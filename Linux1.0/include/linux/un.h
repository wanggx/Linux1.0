#ifndef _LINUX_UN_H
#define _LINUX_UN_H

/* unix协议域地址表示结构体 */
struct sockaddr_un {
	unsigned short sun_family;	/* AF_UNIX */
	char sun_path[108];		/* pathname */
};

#endif /* _LINUX_UN_H */
