
#ifndef APRSC_ERRNO_H
#define APRSC_ERRNO_H

#define APRSC_ERRNO_MAX 12
#define APRSC_UNKNOWN_ERROR		-1
#define CLIOK_PEERS_CLOSING		-2
#define CLIOK_THREAD_SHUTDOWN		-3
#define CLIERR_FD_NUM_INVALID		-4
#define CLIERR_EOF			-5
#define CLIERR_OUTPUT_BUFFER_FULL	-6
#define CLIERR_OUTPUT_WRITE_TIMEOUT	-7
#define CLIERR_UPLINK_LOGIN_PROTO_ERR 	-8
#define CLIERR_UPLINK_LOGIN_NOT_VERIFIED	-9
#define CLIERR_LOGIN_RETRIES			-10
#define CLIERR_LOGIN_TIMEOUT			-11
#define CLIERR_INACTIVITY			-12

extern const char *aprsc_strerror(int er);

#endif
