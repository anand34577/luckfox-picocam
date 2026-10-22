//go:build windows

package main

import "golang.org/x/sys/windows"

// diskSpace returns free and total bytes of the volume holding path (0, 0 when unknown).
func diskSpace(path string) (free, total int64) {
	p, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return 0, 0
	}
	var avail, tot, totFree uint64
	if windows.GetDiskFreeSpaceEx(p, &avail, &tot, &totFree) != nil {
		return 0, 0
	}
	return int64(avail), int64(tot)
}
