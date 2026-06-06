/*
 * uai - KidBright µAI serial toolkit (macOS host side)
 *
 * One self-contained C tool to talk to the board's serial console, replacing
 * the python/pyserial scripts. Usable interactively by a human AND scriptably
 * (exec/push/run print clean stdout and propagate the board's exit code).
 *
 *   uai [-p PORT] [-b BAUD] <command> [args]
 *
 *   monitor [-t]        read-only: print everything the board sends (-t timestamps)
 *   term                interactive terminal (type + read); Ctrl-] to quit
 *   exec "CMD"          run a shell command on the board, print its output, exit rc
 *   push LOCAL REMOTE   upload a file via octal-printf (busybox has no base64), verify
 *   run REMOTE [args]   execute REMOTE on the board, print output, exit rc
 *   deploy LOCAL REMOTE push + chmod +x + run, in one step
 *
 * Defaults: PORT=/dev/cu.usbserial-210 BAUD=115200  (override via -p/-b or
 * env UAI_PORT / UAI_BAUD). Build: cc -O2 -Wall -o bin/uai src/uai.c
 */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

#define DEF_PORT "/dev/cu.usbserial-210"
#define DEF_BAUD 115200
/* Sentinels are quote-split when sent ('UAI''_BEGIN'), so the board's command
 * echo never contains the literal token -- only the real printed output does. */
#define MARK_BEGIN "UAI_BEGIN"
#define MARK_END   "UAI_END_"
#define BYTES_PER_CHUNK 45          /* *4 octal chars = 180 chars/printf line */

/* ----- dynamic buffer ----- */
typedef struct { char *d; size_t len, cap; } buf_t;
static void buf_init(buf_t *b){ b->cap=8192; b->len=0; b->d=malloc(b->cap); b->d[0]=0; }
static void buf_append(buf_t *b, const char *s, size_t n){
    if(b->len+n+1 > b->cap){ while(b->len+n+1>b->cap) b->cap*=2; b->d=realloc(b->d,b->cap); }
    memcpy(b->d+b->len, s, n); b->len+=n; b->d[b->len]=0;
}

/* ----- port ----- */
static speed_t baud_const(int b){
    switch(b){
        case 9600:return B9600; case 19200:return B19200; case 38400:return B38400;
        case 57600:return B57600; case 115200:return B115200; case 230400:return B230400;
        default:return 0;
    }
}
static int open_port(const char *dev, int baud){
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0){ fprintf(stderr,"uai: open %s: %s\n", dev, strerror(errno)); return -1; }
    fcntl(fd, F_SETFL, 0);
    struct termios t;
    if(tcgetattr(fd,&t)){ perror("tcgetattr"); close(fd); return -1; }
    cfmakeraw(&t);
    speed_t s = baud_const(baud);
    if(!s){ fprintf(stderr,"uai: unsupported baud %d\n",baud); close(fd); return -1; }
    cfsetispeed(&t,s); cfsetospeed(&t,s);
    t.c_cflag |= (CLOCAL|CREAD);
    t.c_cflag &= ~CRTSCTS;
    t.c_cflag &= ~PARENB;
    t.c_cflag &= ~CSTOPB;
    t.c_cflag &= ~CSIZE; t.c_cflag |= CS8;
    t.c_iflag &= ~(IXON|IXOFF|IXANY);
    t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
    if(tcsetattr(fd,TCSANOW,&t)){ perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}
static int wait_read(int fd, int timeout_ms){
    fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    return select(fd+1,&rf,NULL,NULL, timeout_ms<0?NULL:&tv);
}
static int write_all(int fd, const char *s, size_t n){
    size_t off=0;
    while(off<n){ ssize_t w=write(fd,s+off,n-off); if(w<=0){ if(errno==EINTR) continue; return -1; } off+=w; }
    return 0;
}
static long ms_since(struct timeval *t0){
    struct timeval now; gettimeofday(&now,NULL);
    return (now.tv_sec-t0->tv_sec)*1000 + (now.tv_usec-t0->tv_usec)/1000;
}
/* read into b until token appears or total_ms elapses; 0 ok, -1 timeout */
static int read_until(int fd, buf_t *b, const char *token, int total_ms){
    struct timeval t0; gettimeofday(&t0,NULL);
    for(;;){
        if(strstr(b->d, token)) return 0;
        if(ms_since(&t0) > total_ms) return -1;
        if(wait_read(fd,150) > 0){
            char tmp[2048]; int n=read(fd,tmp,sizeof tmp);
            if(n>0) buf_append(b,tmp,n);
        }
    }
}
static void board_wake(int fd){
    write_all(fd, "\r\n", 2);
    usleep(150000);
    tcflush(fd, TCIFLUSH);
}

/* run CMD on the board. *out (if non-NULL) gets malloc'd output. returns rc, or -1 */
static int board_exec(int fd, const char *cmd, char **out, int timeout_ms){
    tcflush(fd, TCIFLUSH);
    char *line = malloc(strlen(cmd)+96);
    sprintf(line, "echo '%s''%s'; %s; echo '%s''%s'$?\r\n",
            "UAI", "_BEGIN", cmd, "UAI", "_END_");
    write_all(fd, line, strlen(line));
    free(line);

    buf_t b; buf_init(&b);
    if(read_until(fd,&b,MARK_END,timeout_ms)){ free(b.d); if(out)*out=NULL; return -1; }
    /* grab trailing rc digits + newline */
    for(int i=0;i<6;i++){
        if(wait_read(fd,60)>0){ char tmp[256]; int n=read(fd,tmp,sizeof tmp); if(n>0) buf_append(&b,tmp,n); }
        else break;
    }
    char *begin=strstr(b.d,MARK_BEGIN), *end=strstr(b.d,MARK_END);
    int rc = end ? atoi(end+strlen(MARK_END)) : -1;
    if(out){
        char *os = begin ? strchr(begin,'\n') : NULL;
        if(os) os++;
        if(os && end && os<=end){
            size_t len = end-os;
            while(len>0 && (os[len-1]=='\r'||os[len-1]=='\n')) len--;
            char *o=malloc(len+1); memcpy(o,os,len); o[len]=0; *out=o;
        } else *out = strdup("");
    }
    free(b.d);
    return rc;
}

/* ----- subcommands ----- */
static void print_ts(void){
    struct timeval tv; gettimeofday(&tv,NULL);
    struct tm tmv; localtime_r(&tv.tv_sec,&tmv);
    printf("[%02d:%02d:%02d.%03d] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec/1000));
}
static int cmd_monitor(int fd, int ts){
    fprintf(stderr,"uai: monitoring (read-only). Ctrl-C to quit.\n");
    int line_start=1;
    for(;;){
        if(wait_read(fd,1000)<=0) continue;
        char tmp[2048]; int n=read(fd,tmp,sizeof tmp);
        if(n<=0) continue;
        if(!ts){ fwrite(tmp,1,n,stdout); }
        else for(int i=0;i<n;i++){
            if(line_start){ print_ts(); line_start=0; }
            putchar(tmp[i]);
            if(tmp[i]=='\n') line_start=1;
        }
        fflush(stdout);
    }
}

static struct termios g_saved; static int g_raw=0;
static void restore_stdin(void){ if(g_raw){ tcsetattr(STDIN_FILENO,TCSANOW,&g_saved); g_raw=0; } }
static int cmd_term(int fd){
    fprintf(stderr,"uai: interactive terminal. Ctrl-] to quit.\n");
    if(isatty(STDIN_FILENO)){
        tcgetattr(STDIN_FILENO,&g_saved);
        struct termios r=g_saved; cfmakeraw(&r);
        tcsetattr(STDIN_FILENO,TCSANOW,&r);
        g_raw=1; atexit(restore_stdin);
    }
    for(;;){
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf); FD_SET(STDIN_FILENO,&rf);
        int mx = fd>STDIN_FILENO?fd:STDIN_FILENO;
        if(select(mx+1,&rf,NULL,NULL,NULL)<0){ if(errno==EINTR) continue; break; }
        if(FD_ISSET(STDIN_FILENO,&rf)){
            char tmp[1024]; int n=read(STDIN_FILENO,tmp,sizeof tmp);
            if(n<=0) break;
            for(int i=0;i<n;i++) if((unsigned char)tmp[i]==0x1d){ restore_stdin(); return 0; }
            write_all(fd,tmp,n);
        }
        if(FD_ISSET(fd,&rf)){
            char tmp[2048]; int n=read(fd,tmp,sizeof tmp);
            if(n>0){ fwrite(tmp,1,n,stdout); fflush(stdout); }
        }
    }
    restore_stdin();
    return 0;
}

static int cmd_exec(int fd, const char *cmd){
    board_wake(fd);
    char *out=NULL;
    int rc = board_exec(fd, cmd, &out, 15000);
    if(rc<0){ fprintf(stderr,"uai: exec timed out\n"); return 2; }
    if(out && *out){ fputs(out,stdout); if(out[strlen(out)-1]!='\n') putchar('\n'); }
    free(out);
    return rc;
}

static int host_md5(const char *path, char *hex32){     /* uses macOS /sbin/md5 */
    char c[1100]; snprintf(c,sizeof c,"/sbin/md5 -q '%s' 2>/dev/null",path);
    FILE *p=popen(c,"r"); if(!p) return -1;
    int ok = fscanf(p,"%32s",hex32)==1; pclose(p);
    return ok?0:-1;
}
static int cmd_push(int fd, const char *local, const char *remote){
    FILE *f=fopen(local,"rb");
    if(!f){ fprintf(stderr,"uai: %s: %s\n",local,strerror(errno)); return 2; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *data=malloc(sz); if(fread(data,1,sz,f)!=(size_t)sz){ fclose(f); return 2; } fclose(f);

    board_wake(fd);
    char c[1200];
    snprintf(c,sizeof c,"rm -f '%s'",remote);
    board_exec(fd,c,NULL,5000);

    int nchunks=(sz+BYTES_PER_CHUNK-1)/BYTES_PER_CHUNK, done=0;
    char oct[BYTES_PER_CHUNK*4+1];
    for(long i=0;i<sz;i+=BYTES_PER_CHUNK){
        int j=0;
        for(long k=i;k<i+BYTES_PER_CHUNK && k<sz;k++) j+=sprintf(oct+j,"\\%03o",data[k]);
        snprintf(c,sizeof c,"printf '%s' >> '%s'",oct,remote);
        if(board_exec(fd,c,NULL,5000)!=0){ fprintf(stderr,"\nuai: chunk write failed\n"); free(data); return 2; }
        if(++done % 10 == 0 || done==nchunks){ fprintf(stderr,"\ruai: pushed %d/%d chunks",done,nchunks); }
    }
    fprintf(stderr,"\n");
    free(data);

    /* verify: size + md5 */
    char *out=NULL;
    snprintf(c,sizeof c,"wc -c < '%s'",remote);
    board_exec(fd,c,&out,5000);
    long rsz = out?atol(out):-1; free(out); out=NULL;
    if(rsz!=sz){ fprintf(stderr,"uai: SIZE MISMATCH local=%ld board=%ld\n",sz,rsz); return 2; }

    char lmd5[64]={0};
    if(host_md5(local,lmd5)==0){
        snprintf(c,sizeof c,"busybox md5sum '%s'",remote);
        board_exec(fd,c,&out,5000);
        char bmd5[64]={0}; if(out) sscanf(out,"%63s",bmd5); free(out); out=NULL;
        if(strcmp(lmd5,bmd5)!=0){ fprintf(stderr,"uai: MD5 MISMATCH local=%s board=%s\n",lmd5,bmd5); return 2; }
        fprintf(stderr,"uai: pushed %s -> %s  (%ld bytes, md5 %s OK)\n",local,remote,sz,lmd5);
    } else {
        fprintf(stderr,"uai: pushed %s -> %s  (%ld bytes, size OK; md5 host-check skipped)\n",local,remote,sz);
    }
    return 0;
}

static int cmd_run(int fd, int argc, char **argv){       /* argv = remote [args...] */
    board_wake(fd);
    char cmd[1100]={0}; size_t off=0;
    for(int i=0;i<argc;i++) off+=snprintf(cmd+off,sizeof cmd-off,"%s%s",i?" ":"",argv[i]);
    char *out=NULL;
    int rc=board_exec(fd,cmd,&out,15000);
    if(rc<0){ fprintf(stderr,"uai: run timed out\n"); return 2; }
    if(out && *out){ fputs(out,stdout); if(out[strlen(out)-1]!='\n') putchar('\n'); }
    free(out);
    return rc;
}

static int cmd_deploy(int fd, const char *local, const char *remote){
    if(cmd_push(fd,local,remote)) return 2;
    char c[1100]; snprintf(c,sizeof c,"chmod +x '%s'",remote);
    board_exec(fd,c,NULL,5000);
    char *rargv[1]={(char*)remote};
    return cmd_run(fd,1,rargv);
}

static void usage(void){
    fprintf(stderr,
      "uai - KidBright µAI serial toolkit\n"
      "usage: uai [-p PORT] [-b BAUD] <command> [args]\n"
      "  monitor [-t]         read-only console (-t = timestamps)\n"
      "  term                 interactive terminal (Ctrl-] to quit)\n"
      "  exec \"CMD\"           run a shell command on the board\n"
      "  push LOCAL REMOTE    upload a file (octal-printf + md5 verify)\n"
      "  run REMOTE [args]    execute REMOTE on the board\n"
      "  deploy LOCAL REMOTE  push + chmod +x + run\n"
      "defaults: -p %s -b %d (or env UAI_PORT/UAI_BAUD)\n", DEF_PORT, DEF_BAUD);
}

int main(int argc, char **argv){
    const char *port = getenv("UAI_PORT"); if(!port) port=DEF_PORT;
    const char *be = getenv("UAI_BAUD"); int baud = be?atoi(be):DEF_BAUD;
    signal(SIGPIPE, SIG_IGN);

    int i=1;
    for(; i<argc; i++){
        if(!strcmp(argv[i],"-p") && i+1<argc) port=argv[++i];
        else if(!strcmp(argv[i],"-b") && i+1<argc) baud=atoi(argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){ usage(); return 0; }
        else break;
    }
    if(i>=argc){ usage(); return 1; }
    const char *cmd=argv[i++];

    int fd=open_port(port,baud);
    if(fd<0) return 2;

    int rc=1;
    if(!strcmp(cmd,"monitor")){ int ts=(i<argc && !strcmp(argv[i],"-t")); rc=cmd_monitor(fd,ts); }
    else if(!strcmp(cmd,"term")) rc=cmd_term(fd);
    else if(!strcmp(cmd,"exec")){ if(i>=argc){usage();rc=1;} else rc=cmd_exec(fd,argv[i]); }
    else if(!strcmp(cmd,"push")){ if(i+1>=argc){usage();rc=1;} else rc=cmd_push(fd,argv[i],argv[i+1]); }
    else if(!strcmp(cmd,"run")){ if(i>=argc){usage();rc=1;} else rc=cmd_run(fd,argc-i,&argv[i]); }
    else if(!strcmp(cmd,"deploy")){ if(i+1>=argc){usage();rc=1;} else rc=cmd_deploy(fd,argv[i],argv[i+1]); }
    else { usage(); rc=1; }

    close(fd);
    return rc;
}
