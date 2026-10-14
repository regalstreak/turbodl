// Host-only replacement for http.c: plain sockets, http:// only. Same semantics (manual redirects, Range, Cookie).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../src/common.h"
typedef struct { int fd; uint64_t left; } mstate;
static mstate S[4096]; static int s_next = 1; static pthread_mutex_t ml = PTHREAD_MUTEX_INITIALIZER;
int http_global_init(void){return 0;}
int http_ctx_init(hctx_t*h){memset(h,0,sizeof(*h));h->tmpl=1;return 0;}
void http_close(hctx_t*h,int keep){(void)keep; if(h->req>0){close(S[h->req].fd);h->req=0;}}
void http_ctx_free(hctx_t*h){http_close(h,0);}
static int hdr(const char*hs,const char*n,char*o,size_t on){size_t nl=strlen(n);const char*p=hs;o[0]=0;while((p=strstr(p,"\r\n"))){p+=2;if(!strncasecmp(p,n,nl)&&p[nl]==':'){p+=nl+1;while(*p==' ')p++;size_t k=0;while(p[k]&&p[k]!='\r'&&k<on-1){o[k]=p[k];k++;}o[k]=0;return 1;}}return 0;}
int http_open(hctx_t*h,const char*url0,int64_t from,int64_t to,int*status,uint64_t*clen,uint64_t*total,char*fin,size_t fl){
  char url[2048]; snprintf(url,sizeof url,"%s",url0); *status=0; if(clen)*clen=0; if(total)*total=0; http_close(h,0);
  for(int hop=0;hop<10;hop++){ *status=0;
    char host[256]="",path[2048]="/"; int port=80; const char*p=url+7; const char*sl=strchr(p,'/');
    size_t hl=sl?(size_t)(sl-p):strlen(p); memcpy(host,p,hl); host[hl]=0; if(sl)snprintf(path,sizeof path,"%s",sl);
    char*c=strchr(host,':'); if(c){*c=0;port=atoi(c+1);}
    struct addrinfo hi={0},*ai; hi.ai_family=AF_INET; hi.ai_socktype=SOCK_STREAM; char ps[8]; snprintf(ps,8,"%d",port);
    if(getaddrinfo(host,ps,&hi,&ai)){h->last_err=-1;return -1;}
    int fd=socket(AF_INET,SOCK_STREAM,0); if(connect(fd,ai->ai_addr,ai->ai_addrlen)){freeaddrinfo(ai);close(fd);h->last_err=-2;return -1;} freeaddrinfo(ai);
    char rq[4096]; int n=snprintf(rq,sizeof rq,"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",path,host);
    if(from>=0){ if(to>=0) n+=snprintf(rq+n,sizeof rq-n,"Range: bytes=%lld-%lld\r\n",(long long)from,(long long)to); else n+=snprintf(rq+n,sizeof rq-n,"Range: bytes=%lld-\r\n",(long long)from);}
    { char ck[4096]; if(cookie_lookup(host,ck,sizeof ck)) n+=snprintf(rq+n,sizeof rq-n,"Cookie: %s\r\n",ck); }
    n+=snprintf(rq+n,sizeof rq-n,"\r\n"); send(fd,rq,n,0);
    char hb[8192]; size_t got=0; char*e=NULL;
    while(got<sizeof hb-1){ ssize_t r=recv(fd,hb+got,1,0); if(r<=0)break; got+=r; hb[got]=0; if((e=strstr(hb,"\r\n\r\n")))break;}
    if(!e){close(fd);h->last_err=-3;return -1;}
    int st=0; sscanf(hb,"HTTP/%*s %d",&st); *status=st;
    if(st>=300&&st<400){ char loc[2048]; hdr(hb,"Location",loc,sizeof loc); close(fd); if(loc[0]=='/'){ char o[300]; snprintf(o,sizeof o,"http://%s:%d",host,port); char t[2048]; snprintf(t,sizeof t,"%s%s",o,loc); snprintf(url,sizeof url,"%s",t);} else snprintf(url,sizeof url,"%s",loc); continue;}
    char v[256]; uint64_t cl=0; if(hdr(hb,"Content-Length",v,sizeof v)) cl=strtoull(v,0,10); if(clen)*clen=cl;
    if(total&&hdr(hb,"Content-Range",v,sizeof v)){char*s=strrchr(v,'/'); if(s)*total=strtoull(s+1,0,10);}
    pthread_mutex_lock(&ml); int id=s_next++; if(s_next>=4096)s_next=1; pthread_mutex_unlock(&ml);
    S[id].fd=fd; S[id].left=cl; h->req=id; if(fin)snprintf(fin,fl,"%s",url); return 0;
  }
  return -1;
}
int http_read(hctx_t*h,void*buf,uint32_t n){ if(h->req<=0)return -1; mstate*m=&S[h->req]; if(!m->left)return 0; if(n>m->left)n=(uint32_t)m->left; ssize_t r=recv(m->fd,buf,n,0); if(r<=0)return -1; m->left-=r; return (int)r;}
