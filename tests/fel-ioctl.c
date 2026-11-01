/* SPDX-License-Identifier: MIT */
/* Run on an idle Cube with the reviewed helper loaded, never during playback. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "dvfel_sc2_uapi.h"

static int failures;
static void expect(int result, int error, const char *name)
{
 int actual = result < 0 ? errno : 0;
 if ((error && (result != -1 || actual != error)) || (!error && result != 0)) {
  fprintf(stderr, "FAIL %s: result=%d errno=%d expected=%d\n", name, result, actual, error);
  failures++;
 } else printf("PASS %s\n", name);
}
struct waiter { int fd; int result; int error; };
static void *waiter(void *arg)
{
 struct waiter *w=arg;
 struct dvfel_sc2_done d={.timeout_ms=1000};
 w->result=ioctl(w->fd,DVFEL_IOC_SC2_WAIT_DONE,&d);w->error=errno;
 return NULL;
}
static int start_retry(int fd)
{
 for(int i=0;i<30;i++) {
  int r=ioctl(fd,DVFEL_IOC_SC2_START);
  if(!r || errno!=EBUSY) return r;
  usleep(100000);
 }
 errno=EBUSY;return -1;
}
int main(void)
{
 setbuf(stdout,NULL);
 int a=open("/dev/dvfel",O_RDWR|O_CLOEXEC), b=open("/dev/dvfel",O_RDWR|O_CLOEXEC);
 if(a<0 || b<0){perror("open dvfel");return 2;}
 struct dvfel_sc2_info info={0};
 expect(ioctl(a,DVFEL_IOC_SC2_INFO,&info),0,"INFO");
 if(strcmp(info.version,"2026.10.29.1")){fprintf(stderr,"Wrong helper version\n");return 2;}
 printf("uid=%d helper=%s\n",getuid(),info.version);
 expect(ioctl(a,_IO('F',99)),ENOTTY,"unknown command");
 expect(ioctl(a,_IOR('F',20,int),&info),ENOTTY,"wrong structure size");
 expect(ioctl(a,DVFEL_IOC_SC2_INFO,(void *)1),EFAULT,"invalid user pointer");
 struct dvfel_sc2_done d={0};
 expect(ioctl(b,DVFEL_IOC_SC2_WAIT_DONE,&d),EPERM,"WAIT without session");
 expect(ioctl(a,DVFEL_IOC_SC2_START),0,"START");
 expect(ioctl(b,DVFEL_IOC_SC2_START),EBUSY,"second owner START");
 expect(ioctl(b,DVFEL_IOC_SC2_WAIT_DONE,&d),EPERM,"nonowner completion read");
 expect(ioctl(b,DVFEL_IOC_SC2_STOP),EPERM,"nonowner STOP");
 unsigned flags=3;
 expect(ioctl(b,DVFEL_IOC_SC2_FLUSH,&flags),EPERM,"nonowner FLUSH");
 flags=4;
 expect(ioctl(a,DVFEL_IOC_SC2_FLUSH,&flags),EINVAL,"invalid flush bits");
 struct dvfel_sc2_hint hint={.reserved=1};
 expect(ioctl(a,DVFEL_IOC_SC2_BL_HINT,&hint),EINVAL,"reserved hint field");
 hint.reserved=0;
 expect(ioctl(b,DVFEL_IOC_SC2_BL_HINT,&hint),EPERM,"nonowner hint");
 expect(ioctl(a,DVFEL_IOC_SC2_BL_HINT,&hint),0,"valid hint");
 struct dvfel_sc2_el el={.fd=-1};
 expect(ioctl(b,DVFEL_IOC_SC2_EL_QUEUE,&el),EPERM,"nonowner EL queue");
 expect(ioctl(a,DVFEL_IOC_SC2_EL_QUEUE,&el),EBADF,"invalid dma-buf fd");
 el.reserved=1;
 expect(ioctl(a,DVFEL_IOC_SC2_EL_QUEUE,&el),EINVAL,"reserved EL field");
 el.reserved=0;el.fd=open("/dev/null",O_RDONLY);
 expect(ioctl(a,DVFEL_IOC_SC2_EL_QUEUE,&el),EINVAL,"non-dma-buf fd");close(el.fd);
 d.timeout_ms=1001;
 expect(ioctl(a,DVFEL_IOC_SC2_WAIT_DONE,&d),EINVAL,"unbounded completion wait");
 memset(&d,0xa5,sizeof(d));d.timeout_ms=0;
 expect(ioctl(a,DVFEL_IOC_SC2_WAIT_DONE,&d),0,"empty completion read");
 for(unsigned i=0;i<DVFEL_SC2_MAX_DONE;i++) if(d.id[i]) failures++;
 struct waiter w={.fd=a};pthread_t thread;
 if(pthread_create(&thread,NULL,waiter,&w))return 2;
 usleep(50000);
 expect(ioctl(a,DVFEL_IOC_SC2_STOP),0,"STOP while WAIT sleeps");
 pthread_join(thread,NULL);errno=w.error;
 expect(w.result,EPERM,"old waiter rejected after STOP");
 expect(start_retry(b),0,"next owner after drain");
 close(a);
 expect(ioctl(b,DVFEL_IOC_SC2_BL_HINT,&hint),0,"nonowner close leaves session intact");
 close(b);
 a=open("/dev/dvfel",O_RDWR|O_CLOEXEC);
 expect(start_retry(a),0,"owner close releases session");
 expect(ioctl(a,DVFEL_IOC_SC2_STOP),0,"final STOP");close(a);
 printf("failures=%d\n",failures);return failures?1:0;
}
