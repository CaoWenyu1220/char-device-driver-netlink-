/**
 * Netlink demo.
 * Use with a linux kernel module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h> 
#include <sys/types.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/socket.h>
#include <fcntl.h>
#include <errno.h>

#ifdef TEST_SEC_DEBUG
#define test_printf(fmd, args...) printf(fmd, ##args)
#else
#define test_printf(fmd, args...)
#endif

#define READING_EAGAIN_TIMEOUT 100
#define UTIME_BUF_MAX 8

struct user_tm {
	unsigned long usr_sec;
	unsigned long usr_msecs;
};

int main(){
#define MAX_PAYLOAD 1024 /* maximum payload size*/
#define NETLINK_TRAINING 30
	struct sockaddr_nl src_addr, dest_addr;
	struct nlmsghdr *nlh = NULL;
	struct iovec iov;
	struct msghdr msg;
	int sock_fd, retval, ret, fd=-1, i, maxfd=0;
	fd_set readfds;

	char *dev = "/dev/t-netlink";
	int flag =  O_RDONLY | O_NONBLOCK;
	int reading_again_times;

  struct user_tm user_buff[UTIME_BUF_MAX];
	memset(user_buff, 0, sizeof(user_buff));

	if ( (sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_TRAINING)) < 0 ) {
		perror("socket Failed !!!\n");
		return -1;
	}

	memset(&src_addr, 0, sizeof(src_addr));
	src_addr.nl_family	= AF_NETLINK;/* AF_NETLINK （跟AF_INET对应）*/
	src_addr.nl_pid		= getpid(); /* port ID  （通信端口号）using self pid */
	src_addr.nl_groups	= 0; /* multicast groups mask *//* not in mcast groups */
	if ( bind(sock_fd, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0 ) {
		perror("bind Failed !!!\n");
		return -1;
	}
	
	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.nl_family	= AF_NETLINK;
	dest_addr.nl_pid	= 0; /* to Linux Kernel */
	dest_addr.nl_groups	= 0; /* unicast */
	
	/* 宏NLMSG_SPACE(len)返回不小于NLMSG_LENGTH(len)且字节对齐的最小数值 */
	nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
	/* Fill the netlink message header */
	nlh->nlmsg_len		= NLMSG_SPACE(MAX_PAYLOAD);
	nlh->nlmsg_pid		= getpid(); /* port num using self pid */
	nlh->nlmsg_flags	= 0;
	/* Fill in the netlink message payload */
	strcpy(NLMSG_DATA(nlh), "Ready when you are ?\n");
	
	iov.iov_base		= (void *)nlh;
	iov.iov_len			= nlh->nlmsg_len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_name		= (void *)&dest_addr;
	msg.msg_namelen		= sizeof(dest_addr);
	msg.msg_iov			= &iov;
	msg.msg_iovlen		= 1;

	printf(" Sending message. ...\n");
	sendmsg(sock_fd, &msg, 0);
	
	/* Read message from kernel */
	memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
	printf(" Waiting message. ...\n");

	 memset(user_buff,0,sizeof(user_buff));
   
    if( (fd = open(dev, flag)) < 0 ){
			printf(" Open %s failed. The fd is  %d\n", dev, fd);
			exit(0); 
		}   

	while (1) {
		FD_ZERO(&readfds);
		FD_SET(sock_fd, &readfds);	
		FD_SET(fd, &readfds);
		maxfd = (fd>sock_fd)?(fd):(sock_fd);
		
		retval = select(maxfd+1,&readfds, NULL, NULL, NULL);
		if ( retval > 0 ) {
			if ( FD_ISSET(sock_fd, &readfds) ) {
				if ( (ret = recvmsg(sock_fd, &msg, 0)) <= 0 ) {
					perror("recvmsg\n");
				}
				printf(" Received message payload: %s\n", (char *)NLMSG_DATA(nlh));
				sendmsg(sock_fd, &msg, 0);
			}
			if(FD_ISSET( fd, &readfds)){
				do {
				int ret = read(fd,user_buff,sizeof(user_buff));
				}while(
					ret <0
					&& errno == EAGAIN
					 );
    				if(ret < 0 ){
					perror("read error");
    					continue;
    				}
    				printf("reading return%d\n", ret);
    				for(i=0;i<8;i++)
    				printf("Now.[%5lu.%06lu], count.%d\n",
					  user_buff[i].usr_sec,user_buff[i].usr_msecs,i);
			}
		} else
			printf("Select return.%d\n", retval);
 
	}
	/* Close Netlink Socket */
	close(sock_fd);
	close(fd);
	
	return -1;
}

