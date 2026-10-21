# Security

Please report vulnerabilities privately through GitHub's *Report a vulnerability* (Security tab) rather than a public issue.

Defaults you should know: the board image ships with the SSH login `root` / `luckfox`. PicoCam ties the root password to the admin
password you set at first start and lets you switch SSH and ADB off (System > Security). The portal uses HTTPS with a self-signed
certificate (replaceable), login lockout and an optional IP allow-list. Do not expose a camera directly to the internet.
