package main

import (
	"bytes"
	"crypto/tls"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/smtp"
	"net/textproto"
	"net/url"
	"os"
	"strings"
	"time"
)

const maxAttach = 8 << 20 // bigger pictures are not attached

var notifyClient = &http.Client{
	Timeout:       15 * time.Second,
	CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
}

// send delivers one message through a channel. d and imgPath are optional (health alerts have neither).
func (a *App) send(c Channel, r Rule, msg string, d *Detection, imgPath string) error {
	switch c.Kind {
	case "telegram":
		return sendTelegram(c.Config, msg, imgPath)
	case "ntfy":
		return sendNtfy(c.Config, r.Name, msg, imgPath)
	case "http":
		body := map[string]any{"rule": r.Name, "message": msg}
		if d != nil {
			body["detection"] = d
		}
		status, err := deliver(c.Config["url"], c.Config["secret"], "alert", body)
		if err != nil {
			return cleanErr(err, "")
		}
		if status/100 != 2 {
			return fmt.Errorf("HTTP %d", status)
		}
		return nil
	case "email":
		return sendEmail(c.Config, r.Name, msg, imgPath)
	}
	return errors.New("unknown channel kind")
}

// cleanErr drops the request URL from network errors (a Telegram URL contains the bot token) and the secret.
func cleanErr(err error, secret string) error {
	var ue *url.Error
	if errors.As(err, &ue) {
		err = ue.Err
	}
	s := err.Error()
	if secret != "" {
		s = strings.ReplaceAll(s, secret, "***")
	}
	return errors.New(s)
}

func readImage(path string) ([]byte, string) {
	if path == "" {
		return nil, ""
	}
	if fi, err := os.Stat(path); err != nil || fi.Size() > maxAttach {
		return nil, ""
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, ""
	}
	return b, "alert.jpg"
}

func sendTelegram(cfg map[string]string, msg, imgPath string) error {
	token := cfg["bot_token"]
	api := "https://api.telegram.org/bot" + token + "/"
	var req *http.Request
	img, name := readImage(imgPath)
	if img != nil {
		caption := msg
		if len([]rune(caption)) > 1000 {
			caption = string([]rune(caption)[:1000])
		}
		var buf bytes.Buffer
		mw := multipart.NewWriter(&buf)
		mw.WriteField("chat_id", cfg["chat_id"])
		mw.WriteField("caption", caption)
		fw, _ := mw.CreateFormFile("photo", name)
		fw.Write(img)
		mw.Close()
		req, _ = http.NewRequest("POST", api+"sendPhoto", &buf)
		req.Header.Set("Content-Type", mw.FormDataContentType())
	} else {
		body, _ := json.Marshal(map[string]string{"chat_id": cfg["chat_id"], "text": msg})
		req, _ = http.NewRequest("POST", api+"sendMessage", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := notifyClient.Do(req)
	if err != nil {
		return cleanErr(err, token)
	}
	defer resp.Body.Close()
	var out struct {
		OK          bool   `json:"ok"`
		Description string `json:"description"`
	}
	json.NewDecoder(io.LimitReader(resp.Body, 64<<10)).Decode(&out)
	if !out.OK {
		if out.Description == "" {
			out.Description = fmt.Sprintf("HTTP %d", resp.StatusCode)
		}
		return errors.New(strings.ReplaceAll(out.Description, token, "***"))
	}
	return nil
}

func sendNtfy(cfg map[string]string, title, msg, imgPath string) error {
	var req *http.Request
	img, _ := readImage(imgPath)
	if img != nil {
		req, _ = http.NewRequest("PUT", cfg["url"], bytes.NewReader(img))
		req.Header.Set("Filename", "alert.jpg")
		req.Header.Set("Message", mime.QEncoding.Encode("utf-8", msg))
	} else {
		req, _ = http.NewRequest("POST", cfg["url"], strings.NewReader(msg))
	}
	req.Header.Set("Title", mime.QEncoding.Encode("utf-8", title))
	if t := cfg["token"]; t != "" {
		req.Header.Set("Authorization", "Bearer "+t)
	}
	resp, err := notifyClient.Do(req)
	if err != nil {
		return cleanErr(err, cfg["token"])
	}
	resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	return nil
}

// oneLine removes anything that could add mail headers.
func oneLine(s string) string {
	return strings.NewReplacer("\r", " ", "\n", " ").Replace(s)
}

func sendEmail(cfg map[string]string, subject, msg, imgPath string) error {
	port := cfg["port"]
	sec := cfg["security"]
	if port == "" {
		port = map[string]string{"tls": "465", "none": "25"}[sec]
		if port == "" {
			port = "587"
		}
	}
	addr := net.JoinHostPort(cfg["host"], port)
	from := oneLine(cfg["from"])
	var to []string
	for _, t := range strings.Split(cfg["to"], ",") {
		if t = strings.TrimSpace(oneLine(t)); t != "" {
			to = append(to, t)
		}
	}
	if len(to) == 0 {
		return errors.New("no recipient")
	}

	var body bytes.Buffer
	boundary := fmt.Sprintf("edgeai-%d", time.Now().UnixNano())
	fmt.Fprintf(&body, "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\nMIME-Version: 1.0\r\n", from,
		strings.Join(to, ", "), mime.QEncoding.Encode("utf-8", "EdgeAI: "+oneLine(subject)), time.Now().Format(time.RFC1123Z))
	img, _ := readImage(imgPath)
	if img == nil {
		fmt.Fprintf(&body, "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n%s\r\n", wrap76(base64.StdEncoding.EncodeToString([]byte(msg))))
	} else {
		fmt.Fprintf(&body, "Content-Type: multipart/mixed; boundary=%s\r\n\r\n", boundary)
		mw := multipart.NewWriter(&body)
		mw.SetBoundary(boundary)
		p, _ := mw.CreatePart(textproto.MIMEHeader{"Content-Type": {"text/plain; charset=utf-8"}, "Content-Transfer-Encoding": {"base64"}})
		p.Write([]byte(wrap76(base64.StdEncoding.EncodeToString([]byte(msg)))))
		p, _ = mw.CreatePart(textproto.MIMEHeader{"Content-Type": {"image/jpeg"}, "Content-Transfer-Encoding": {"base64"},
			"Content-Disposition": {`attachment; filename="alert.jpg"`}})
		p.Write([]byte(wrap76(base64.StdEncoding.EncodeToString(img))))
		mw.Close()
	}

	var auth smtp.Auth
	if cfg["username"] != "" {
		auth = smtp.PlainAuth("", cfg["username"], cfg["password"], cfg["host"])
	}
	if sec == "tls" {
		conn, err := tls.DialWithDialer(&net.Dialer{Timeout: 15 * time.Second}, "tcp", addr, &tls.Config{ServerName: cfg["host"]})
		if err != nil {
			return cleanErr(err, cfg["password"])
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(30 * time.Second))
		cl, err := smtp.NewClient(conn, cfg["host"])
		if err != nil {
			return cleanErr(err, cfg["password"])
		}
		defer cl.Close()
		if auth != nil {
			if err := cl.Auth(auth); err != nil {
				return cleanErr(err, cfg["password"])
			}
		}
		if err := cl.Mail(from); err != nil {
			return cleanErr(err, cfg["password"])
		}
		for _, t := range to {
			if err := cl.Rcpt(t); err != nil {
				return cleanErr(err, cfg["password"])
			}
		}
		w, err := cl.Data()
		if err != nil {
			return cleanErr(err, cfg["password"])
		}
		w.Write(body.Bytes())
		if err := w.Close(); err != nil {
			return cleanErr(err, cfg["password"])
		}
		return cl.Quit()
	}
	// "starttls" and "none": SendMail upgrades to TLS when the server offers it, and PlainAuth refuses to
	// send the password over an unencrypted link to anything but localhost.
	if err := smtp.SendMail(addr, auth, from, to, body.Bytes()); err != nil {
		return cleanErr(err, cfg["password"])
	}
	return nil
}

func wrap76(s string) string {
	var b strings.Builder
	for len(s) > 76 {
		b.WriteString(s[:76] + "\r\n")
		s = s[76:]
	}
	return b.String() + s + "\r\n"
}
