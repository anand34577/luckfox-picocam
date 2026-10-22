//go:build !windows

package main

import "syscall"

// diskSpace returns free and total bytes of the filesystem holding path (0, 0 when unknown).
func diskSpace(path string) (free, total int64) {
	var st syscall.Statfs_t
	if syscall.Statfs(path, &st) != nil {
		return 0, 0
	}
	return int64(st.Bavail) * int64(st.Bsize), int64(st.Blocks) * int64(st.Bsize)
}
