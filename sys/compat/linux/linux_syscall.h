/*
 * System call numbers.
 *
 * DO NOT EDIT-- this file is automatically generated.
 * created from	OpenBSD: syscalls.master,v 1.5 1996/04/28 07:38:24 etheisen Exp 
 */

#define	LINUX_SYS_syscall	0
#define	LINUX_SYS_exit	1
#define	LINUX_SYS_fork	2
#define	LINUX_SYS_read	3
#define	LINUX_SYS_write	4
#define	LINUX_SYS_open	5
#define	LINUX_SYS_close	6
#define	LINUX_SYS_waitpid	7
#define	LINUX_SYS_creat	8
#define	LINUX_SYS_link	9
#define	LINUX_SYS_unlink	10
#define	LINUX_SYS_execve	11
#define	LINUX_SYS_chdir	12
#define	LINUX_SYS_time	13
#define	LINUX_SYS_mknod	14
#define	LINUX_SYS_chmod	15
#define	LINUX_SYS_chown	16
#define	LINUX_SYS_break	17
				/* 18 is obsolete ostat */
#define	LINUX_SYS_lseek	19
#define	LINUX_SYS_getpid	20
#define	LINUX_SYS_mount	21
#define	LINUX_SYS_umount	22
#define	LINUX_SYS_setuid	23
#define	LINUX_SYS_getuid	24
#define	LINUX_SYS_alarm	27
				/* 28 is obsolete ofstat */
#define	LINUX_SYS_pause	29
#define	LINUX_SYS_utime	30
#define	LINUX_SYS_access	33
#define	LINUX_SYS_sync	36
#define	LINUX_SYS_kill	37
#define	LINUX_SYS_rename	38
#define	LINUX_SYS_mkdir	39
#define	LINUX_SYS_rmdir	40
#define	LINUX_SYS_dup	41
#define	LINUX_SYS_pipe	42
#define	LINUX_SYS_times	43
#define	LINUX_SYS_brk	45
#define	LINUX_SYS_setgid	46
#define	LINUX_SYS_getgid	47
#define	LINUX_SYS_signal	48
#define	LINUX_SYS_geteuid	49
#define	LINUX_SYS_getegid	50
#define	LINUX_SYS_acct	51
#define	LINUX_SYS_ioctl	54
#define	LINUX_SYS_fcntl	55
#define	LINUX_SYS_setpgid	57
#define	LINUX_SYS_oldolduname	59
#define	LINUX_SYS_umask	60
#define	LINUX_SYS_chroot	61
#define	LINUX_SYS_dup2	63
#define	LINUX_SYS_getppid	64
#define	LINUX_SYS_getpgrp	65
#define	LINUX_SYS_setsid	66
#define	LINUX_SYS_sigaction	67
#define	LINUX_SYS_siggetmask	68
#define	LINUX_SYS_sigsetmask	69
#define	LINUX_SYS_setreuid	70
#define	LINUX_SYS_setregid	71
#define	LINUX_SYS_sigsuspend	72
#define	LINUX_SYS_sigpending	73
#define	LINUX_SYS_sethostname	74
#define	LINUX_SYS_setrlimit	75
#define	LINUX_SYS_getrlimit	76
#define	LINUX_SYS_getrusage	77
#define	LINUX_SYS_gettimeofday	78
#define	LINUX_SYS_settimeofday	79
#define	LINUX_SYS_getgroups	80
#define	LINUX_SYS_setgroups	81
#define	LINUX_SYS_oldselect	82
#define	LINUX_SYS_symlink	83
#define	LINUX_SYS_olstat	84
#define	LINUX_SYS_readlink	85
#define	LINUX_SYS_uselib	86
#define	LINUX_SYS_swapon	87
#define	LINUX_SYS_reboot	88
#define	LINUX_SYS_readdir	89
#define	LINUX_SYS_mmap	90
#define	LINUX_SYS_munmap	91
#define	LINUX_SYS_truncate	92
#define	LINUX_SYS_ftruncate	93
#define	LINUX_SYS_fchmod	94
#define	LINUX_SYS_fchown	95
#define	LINUX_SYS_getpriority	96
#define	LINUX_SYS_setpriority	97
#define	LINUX_SYS_profil	98
#define	LINUX_SYS_statfs	99
#define	LINUX_SYS_fstatfs	100
#define	LINUX_SYS_ioperm	101
#define	LINUX_SYS_socketcall	102
#define	LINUX_SYS_setitimer	104
#define	LINUX_SYS_getitimer	105
#define	LINUX_SYS_stat	106
#define	LINUX_SYS_lstat	107
#define	LINUX_SYS_fstat	108
#define	LINUX_SYS_olduname	109
#define	LINUX_SYS_iopl	110
#define	LINUX_SYS_wait4	114
#define	LINUX_SYS_ipc	117
#define	LINUX_SYS_fsync	118
#define	LINUX_SYS_sigreturn	119
#define	LINUX_SYS_setdomainname	121
#define	LINUX_SYS_uname	122
#define	LINUX_SYS_modify_ldt	123
#define	LINUX_SYS_mprotect	125
#define	LINUX_SYS_sigprocmask	126
#define	LINUX_SYS_getpgid	132
#define	LINUX_SYS_fchdir	133
#define	LINUX_SYS_personality	136
#define	LINUX_SYS_llseek	140
#define	LINUX_SYS_getdents	141
#define	LINUX_SYS_select	142
#define	LINUX_SYS_flock	143
#define	LINUX_SYS_msync	144
#define	LINUX_SYS_readv	145
#define	LINUX_SYS_writev	146
#define	LINUX_SYS_getsid	147
#define	LINUX_SYS_fdatasync	148
#define	LINUX_SYS___sysctl	149
#define	LINUX_SYS_mlock	150
#define	LINUX_SYS_munlock	151
#define	LINUX_SYS_MAXSYSCALL	164
