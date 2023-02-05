/*
have vim work
restore underline
make utf8 aware
line overflow?
*/
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

const char *pathcopilotdist = NULL; // hard-coded path to try
const char verbose = 0;
const char verbosecopilot = 0; // 1 for messages only, 2 +parsing/waiting
const char verbosetranscript = 0;

// Terminal settings to restore
struct termios oldtermios;
char gottermios = 0;
int fdterm = -1;

// Copilot process
int fdscp[4];
pid_t pidcp = -1;
enum errcopilot {ECPNONE,ECPUNKNOWN,ECPAUTHSTR,ECPAUTH,ECPAUTHGETCODEURI,ECPREADEOF} errcopilot = 0;
char *serrcopilot = NULL;
atomic_char mainexited = 0;

// Transcript
pthread_mutex_t muttranscript = PTHREAD_MUTEX_INITIALIZER;
size_t sizetranscript=0, ntranscript=0;
char *transcript = NULL;
atomic_ulong vtranscript = 0;
pthread_cond_t condtranscript = PTHREAD_COND_INITIALIZER;
size_t itranscriptcur = 0;

// Completion
pthread_mutex_t mutcompletion = PTHREAD_MUTEX_INITIALIZER;
size_t sizecompletion=0, ncompletion=0;
char *completion = NULL;
unsigned vtranscriptcompletion = -1;
int fdscompletion[2];
unsigned vtranscriptcompletionshown = -1;
char acceptedcompletion = 0;
atomic_char ison = 1;

/* sprintf into a statically allocated buffer */
char*
sprintfs(const char *fmt, ...)
{
	static _Thread_local size_t sizeret=0; static _Thread_local char *ret=NULL;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap); if (n<0) { perror("cpwrap: vsnprintf"); exit(1); }
	va_end(ap);
	if (sizeret<n+1 && !(ret=realloc(ret,sizeret=(n+1)*2))) { perror("cpwrap: realloc"); exit(1); }
	va_start(ap, fmt);
	n = vsnprintf(ret, sizeret, fmt, ap); if (n<0) { perror("cpwrap: vsnprintf"); exit(1); }
	va_end(ap);
	return ret;
}

/* Returns NULL if doesn't exist */
char*
readstr(char *path)
{
	FILE *f = fopen(path, "r"); if (!f && errno!=ENOENT) { fprintf(stderr,"cpwwrap: fopen %s: %s\n",path,strerror(errno)); exit(1); }
	if (!f) return NULL;
	struct stat st; if (fstat(fileno(f),&st)) { fprintf(stderr,"cpwwrap: fstat %s: %s\n",path,strerror(errno)); exit(1); }
	char *s = malloc(st.st_size+1); if (!s) { fprintf(stderr,"cpwwrap: malloc %s: %s\n",path,strerror(errno)); exit(1); }
	if (fread(s,1,st.st_size,f) != st.st_size) { fprintf(stderr,"cpwwrap: fread %s short read\n",path); exit(1); }
	if (ferror(f) || fclose(f)) { fprintf(stderr,"cpwwrap: fread/fclose %s: %s\n",path,strerror(errno)); exit(1); }
	s[st.st_size] = 0;
	return s;
}

void
writestr(char *path, char *s)
{
	FILE *f = fopen(path, "w"); if (!f) { fprintf(stderr,"mobilecode: fopen writestr %s: %s\n",path,strerror(errno)); exit(1); }
	fwrite(s, 1, strlen(s), f);
	if (ferror(f) || fclose(f)) { fprintf(stderr,"mobilecode: fwrite/fclose writestr %s: %s\n",path,strerror(errno)); exit(1); }
}

void
cleanup(void)
{
	kill(pidcp, SIGTERM);
	waitpid(pidcp, NULL, 0);
	if (gottermios && tcsetattr(fdterm,TCSANOW,&oldtermios)) perror("cpwrap: tcsetattr");
}

uint64_t
getms(void)
{
	struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("cpwrap: clock_gettime"); exit(1); }
	return ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

char*
eatvaluejson(char *json)
{
	if (!json) return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;

	// Object
	if (*json == '{') {
		while (*++json && strchr(" \n\r\t",*json));
		if (*json != '}') while (1) {
			if (*json != '"') return NULL;
			if (!(json=eatvaluejson(json))) return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
			if (*json++ != ':') return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
			if (!(json=eatvaluejson(json))) return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
			if (*json == '}') break;
			if (*json++ != ',') return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
		}
		json++;

	// Array
	} else if (*json == '[') {
		while (*++json && strchr(" \n\r\t",*json));
		if (*json != ']') while (1) {
			if (!(json=eatvaluejson(json))) return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
			if (*json == ']') break;
			if (*json++ != ',') return NULL;
			while (*json && strchr(" \n\r\t",*json)) json++;
		}
		json++;

	// String
	} else if (*json == '"') {
		while (*++json && *json!='"') if (*json == '\\') json++;
		if (*json++ != '"') return NULL;

	// Number
	} else if (strchr("0123456789-",*json)) {
		if (*json == '-') json++;
		while (*json && strchr("0123456789",*json)) json++;
		if (*json == '.') while (*++json && strchr("0123456789",*json));
		if (strchr("eE",*json)) {
			if (strchr("+-",*++json)) json++;
			while (*json && strchr("0123456789",*json)) json++;
		}

	// Constants
	} else if (!strncmp(json,"true",4)) json += 4;
	else if (!strncmp(json,"false",5)) json += 5;
	else if (!strncmp(json,"null",4)) json += 4;

	else return NULL;
	return json;
}

char*
getkjson(char *json, char *k)
{
	if (!json) return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;
	if (*json++ != '{') return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;

	// Loop through keys looking for k
	while (1) {
		if (*json++ != '"') return NULL;

		// Loop until end of key, jumping if no match
		char *pk=k; while (*pk) {
			if (*json == '\\') {
				if (*++json == 'u') {
					json++;
					for (int i=0; i<4; i++) if (!strchr("0123456789abcdefABCDEF",json[i])) return NULL;
					unsigned long cp=0; for (int i=0; i<4; i++,json++) {
						cp <<= 4;
						if (*json>='0' && *json<='9') cp |= *json-'0';
						else if (*json>='a' && *json<='f') cp |= *json-'a'+10;
						else if (*json>='A' && *json<='F') cp |= *json-'A'+10;
					}
					if (!cp) return NULL;
					if (cp < 0x80) { if (*pk++ != cp) goto eatstringgetkjson; }
					else if (cp < 0x800) { if (*pk++!=0xc0+(cp>>6) || *pk++!=0x80+(cp&0x3f)) goto eatstringgetkjson; }
					else if (cp < 0x10000) { if (*pk++!=0xe0+(cp>>12) || *pk++!=0x80+((cp>>6)&0x3f) || *pk++!=0x80+(cp&0x3f)) goto eatstringgetkjson; }
					else if (cp <= 0x10ffff) { if (*pk++!=0xf0+(cp>>18) || *pk++!=0x80+((cp>>12)&0x3f) || *pk++!=0x80+((cp>>6)&0x3f) || *pk++!=0x80+(cp&0x3f)) goto eatstringgetkjson; }
					else return NULL;
				} else if (*json == '"' || *json == '\\' || *json == '/') {
					if (*pk++ != *json++) goto eatstringgetkjson;
				} else {
					const char *escs = "bfnrt";
					char *pesc = strchr(escs, *json++);
					if (!pesc) return NULL;
					if (*pk++ != "\b\f\n\r\t"[pesc-escs]) goto eatstringgetkjson;
				}
			} else if (*json=='"' || !*json || *pk++!=*json++) goto eatstringgetkjson;
		}
		if (*json++ != '"') goto eatstringgetkjson;
		while (*json && strchr(" \n\r\t",*json)) json++;
		if (*json++ != ':') return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
		return json;

		// Move to next key
		eatstringgetkjson:;
		for (; *json && *json!='"'; json++) if (*json == '\\') json++;
		if (*json++ != '"') return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
		if (*json++ != ':') return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
		if (!(json=eatvaluejson(json))) return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
		if (*json++ != ',') return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
	}
	return NULL;
}

char*
getksjson(char *json, char *k)
{

	// Get string value
	json = getkjson(json, k);
	if (!json) return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;
	if (*json++ != '"') return NULL;

	// Initialize s
	static size_t sizes=0; static char *s=NULL;
	if (!sizes && !(s=malloc(sizes=256))) { perror("cpwrap: getksjson: malloc"); exit(1); }
	size_t ns = 0;

	// Decode string
	while (*json != '"') {
		if (!*json) return NULL;
		if (sizes<ns+5 && !(s=realloc(s,sizes=ns*2))) { perror("cpwrap: getksjson: realloc"); exit(1); }
		if (*json == '\\') {
			if (*++json == 'u') {
				json++;
				for (int i=0; i<4; i++) if (!strchr("0123456789abcdefABCDEF",json[i])) return NULL;
				unsigned long cp=0; for (int i=0; i<4; i++,json++) {
					cp <<= 4;
					if (*json>='0' && *json<='9') cp |= *json-'0';
					else if (*json>='a' && *json<='f') cp |= *json-'a'+10;
					else if (*json>='A' && *json<='F') cp |= *json-'A'+10;
				}
				if (!cp) return NULL;
				if (cp < 0x80) s[ns++]=cp;
				else if (cp < 0x800) s[ns++]=0xc0+(cp>>6), s[ns++]=0x80+(cp&0x3f);
				else if (cp < 0x10000) s[ns++]=0xe0+(cp>>12), s[ns++]=0x80+((cp>>6)&0x3f), s[ns++]=0x80+(cp&0x3f);
				else if (cp <= 0x10ffff) s[ns++]=0xf0+(cp>>18), s[ns++]=0x80+((cp>>12)&0x3f), s[ns++]=0x80+((cp>>6)&0x3f), s[ns++]=0x80+(cp&0x3f);
				else return NULL;
			} else if (*json == '"' || *json == '\\' || *json == '/') {
				s[ns++] = *json++;
			} else {
				const char *escs = "bfnrt";
				char *pesc = strchr(escs, *json++);
				if (!pesc) return NULL;
				s[ns++] = "\b\f\n\r\t"[pesc-escs];
			}
		} else s[ns++] = *json++;
	}
	s[ns] = 0;

	return s;
}

long
getknjson(char *json, char *k)
{
	json = getkjson(json, k);
	if (!json) return LONG_MAX;
	while (*json && strchr(" \n\r\t",*json)) json++;
	if (*json<'0' || *json>'9') return LONG_MAX;
	long n=0; while (*json>='0' &&*json<='9') n=n*10+*json++-'0';
	return n;
}

char*
getijson(char *json, unsigned iget)
{
	if (!json) return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;
	if (*json++ != '[') return NULL;
	while (*json && strchr(" \n\r\t",*json)) json++;
	if (*json == ']') return NULL;
	for (unsigned iarr=0;; iarr++) {
		if (iarr == iget) return json;
		if (!(json=eatvaluejson(json))) return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
		if (*json++ != ',') return NULL;
		while (*json && strchr(" \n\r\t",*json)) json++;
	}
	return NULL;
}

char
willblockread(int fd)
{
	while (1) {
		int res = poll(&(struct pollfd){.fd=fd,.events=POLLIN}, 1, 0); if (res==-1 && errno!=EINTR && errno!=EAGAIN) { fprintf(stderr,"cpwrap: poll fd %d: %s\n",fd,strerror(errno)); exit(1); }
		if (res == -1) continue;
		return res == 0;
	}
}

char
willblockwrite(int fd)
{
	while (1) {
		int res = poll(&(struct pollfd){.fd=fd,.events=POLLOUT}, 1, 0); if (res==-1 && errno!=EINTR && errno!=EAGAIN) { fprintf(stderr,"cpwrap: poll fd %d: %s\n",fd,strerror(errno)); exit(1); }
		if (res == -1) continue;
		return res == 0;
	}
}

void
writeall(int fd, size_t nbuf, const char *buf)
{
	for (size_t ibuf=0; ibuf<nbuf; ) {
		ssize_t n = write(fd, buf+ibuf, nbuf-ibuf); if (n==-1 && errno!=EINTR && errno!=EAGAIN) { fprintf(stderr,"cpwrap: write fd %d: %s\n",fd,strerror(errno)); exit(1); }
		if (n == -1) continue;
		if (!n) { fprintf(stderr,"cpwrap: write fd %d writeall: EOF\n",fd); exit(1); }
		ibuf += n;
	}
}

void
writes(int fd, const char *s)
{
	while (*s) {
		ssize_t n = write(fd, s, strlen(s)); if (n==-1 && errno!=EINTR && errno!=EAGAIN) { fprintf(stderr,"cpwrap: write fd %d: %s\n",fd,strerror(errno)); exit(1); }
		if (n == -1) continue;
		if (!n) { fprintf(stderr,"cpwrap: write fd %d writes: EOF\n",fd); exit(1); }
		s += n;
	}
}

struct writecond {
	int fd;
	pthread_mutex_t *pmut;
	pthread_cond_t *pcond;
	int (*fn)(void*);
	void *arg;
	char want;
};

/* Write to an fd when a function is true, waiting for changes on cond. */
void*
dowritecond(void *arg)
{
	int res;
	struct writecond *wc = (struct writecond*)arg;
	if ((res=pthread_mutex_lock(wc->pmut))) { fprintf(stderr,"cpwrap: pthread_mutex_lock writecond: %s\n",strerror(res)); exit(1); }
	while (1) {

		// Wait until it is wanted
		while (1) {
			if (wc->want) break;
			if ((res=pthread_cond_wait(wc->pcond,wc->pmut))) { fprintf(stderr,"cpwrap: pthread_cond_wait writecond: %s\n",strerror(res)); exit(1); }
		}

		// Wait for fn to return true
		while (1) {
			if (wc->fn(wc->arg)) break;
			if ((res=pthread_cond_wait(wc->pcond,wc->pmut))) { fprintf(stderr,"cpwrap: pthread_cond_wait writecond2: %s\n",strerror(res)); exit(1); }
		}

		// Notify
		wc->want = 0;
		while (1) {
			ssize_t w = write(wc->fd, "", 1); if (w==-1 && errno!=EINTR) { perror("cpwrap: dowritecond write"); exit(1); }
			if (w != -1) break;
		}
	}
	return NULL;
}

int
isvtranscriptdifferent(void *arg)
{
	return atomic_load(&vtranscript) != *(unsigned long*)arg;
}

void
msgcopilot(char *msg)
{
	if (errcopilot) return;
	int nmsg = strlen(msg);
	char len[64]; int nlen=snprintf(len, sizeof(len), "Content-Length: %d\r\n\r\n", nmsg);
	if (verbose || verbosecopilot) fprintf(stderr,"\n\33[33mwriting to copilot:\n%s\33[0m\n",msg);
	writeall(fdscp[1], nlen, len);
	writeall(fdscp[1], nmsg, msg);
}

/* 0=EAGAIN or done, 1=err */
int
getmsgcopilotideagain(long id, char **pmsg)
{
	if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mtrying to get a message from copilot with id %ld\33[0m\n",id);
	*pmsg = NULL;
	if (errcopilot) return 1;
	static size_t sizebuf=0,nbuf=0; static char *buf=NULL;

	// Loop forever getting messages
	while (1) {

		// Macro to fill to a certain point
		#define FILL(nexpr) { \
			size_t n = nexpr; \
			if (sizebuf<nbuf+n+1 && !(buf=realloc(buf,sizebuf=(nbuf+n+1)*2))) { fprintf(stderr,"cpwrap: realloc buf %zu: %s\n",sizebuf,strerror(errno)); exit(1); } \
			while (nbuf < n) { \
				ssize_t nread = read(fdscp[2],buf+nbuf,n); if (nread==-1 && errno!=EINTR && errno!=EAGAIN) { perror("cpwrap: read copilot"); errcopilot=1; return 1; } \
				if ((verbose||verbosecopilot>=2) && nread==-1 && errno==EAGAIN) fprintf(stderr,"\33[33mread copilot: EAGAIN\33[0m\n"); \
				if (nread==-1 && errno==EAGAIN) return 0; \
				if (!nread) { errcopilot=ECPREADEOF; return 1; } \
				nbuf += nread; \
			} \
		}

		// Get content length header
		size_t ibuf = 0;
		char *want="Content-Length: "; int nwant=strlen(want);
		if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mfilling for content-length\33[0m\n");
		FILL(ibuf+nwant)
		if (memcmp(buf+ibuf,want,nwant)) { fprintf(stderr,"cpwrap: copilot: bad header\n"); errcopilot=1; return 1; }
		ibuf += nwant;
		size_t nnum=0; while (1) {
			buf[nbuf] = 0;
			for (; ibuf+nnum<nbuf; nnum++) if (!strchr("0123456789",buf[ibuf+nnum])) goto gotcontentlen;
			if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mfilling for content-length #\33[0m\n");
			FILL(ibuf+nnum+4)
		} gotcontentlen:;
		if (!nnum) { fprintf(stderr,"cpwrap: copilot: bad header content-length #\n"); errcopilot=1; return 1; }
		unsigned len=0; for (size_t i=0; i<nnum; i++) len = len*10 + buf[ibuf++]-'0';
		if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mcontent-length: %u\nfilling for \\r\\n\\r\\n\33[0m\n",len);
		FILL(ibuf+4)
		if (memcmp(buf+ibuf,"\r\n\r\n",4)) { fprintf(stderr,"cpwrap: copilot: bad header no CRNLCRNL\n"); errcopilot=1; return 1; }
		ibuf += 4;

		// Get content
		if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mfilling for message (%zu/%u)\33[0m\n",nbuf-ibuf,len);
		FILL(ibuf+len)
		#undef FILL
		if (verbose || verbosecopilot) fprintf(stderr,"\n\33[33mmsg from copilot:\n%.*s\33[0m\n", (int)len, buf+ibuf);

		// Get id
		char cend0 = buf[ibuf+len];
		buf[ibuf+len] = 0;
		long idmsg = getknjson(buf+ibuf, "id");

		// Make a copy if it's our message
		static size_t sizeret=0; static char *ret=NULL;
		if (idmsg == id) {
			if (sizeret<len+1 && !(ret=realloc(ret,sizeret=(len+1)*2))) { fprintf(stderr,"cpwrap: realloc ret %zu: %s\n",sizeret,strerror(errno)); exit(1); }
			memcpy(ret, buf+ibuf, len);
			ret[len] = 0;
		}

		// Move past message in buffer
		buf[ibuf+len] = cend0;
		memmove(buf, buf+ibuf+len, nbuf-=ibuf+len);

		// Return or error if missed
		if (idmsg == id) { *pmsg=ret; return 0; }
		if (idmsg!=LONG_MAX && idmsg>id) { fprintf(stderr,"cpwrap: copilot: missed id\n"); errcopilot=1; return 1; }
	}
	return 1;
}

char*
getmsgcopilotid(long id)
{
	char *ret; getmsgcopilotideagain(id, &ret);
	return ret;
}

int
getmsgcopilotidnonblock(long id, char **pmsg)
{
	int flags0 = fcntl(fdscp[2], F_GETFL, 0); if (flags0 == -1) { perror("cpwrap: fcntl get flags"); exit(1); }
	if (fcntl(fdscp[2],F_SETFL,flags0|O_NONBLOCK) == -1) { perror("cpwrap: fcntl copilot"); exit(1); }
	int ret = getmsgcopilotideagain(id, pmsg);
	if (fcntl(fdscp[2],F_SETFL,flags0) == -1) { perror("cpwrap: fcntl copilot"); exit(1); }
	return ret;
}

void*
docopilot(void *arg)
{
	//return NULL;
	int res;

	// Send initial messages
	msgcopilot("{\"jsonrpc\":\"2.0\",\"params\":{\"capabilities\":{}},\"method\":\"initialize\",\"id\":1}");
	msgcopilot("{\"params\":{\"editorInfo\":{\"name\":\"cpwrap\",\"version\":\"1\"},\"editorPluginInfo\":{\"name\":\"cpwrap\",\"version\":\"1\"}},\"id\":2,\"method\":\"setEditorInfo\",\"jsonrpc\":\"2.0\"}");

	// Create dummy file
	char *tmpdir = getenv("TMPDIR"); if (!tmpdir) tmpdir = "/tmp";
	char *pathdummy = sprintfs("%s/shell_session.txt", tmpdir);
	int fddummy = open(pathdummy, O_RDWR|O_CREAT|O_EXCL, 0644);
	if (fddummy != -1) close(fddummy);
	int nesc=0; for (char *p=pathdummy; *p; p++) if (*p == '/') nesc++;
	char *pathdummyesc = malloc(strlen(pathdummy)+nesc+1); if (!pathdummyesc) { perror("cpwrap: malloc"); exit(1); }
	char *p=pathdummyesc; for (char *q=pathdummy; *q; q++) { if (*q == '/') *p++='\\'; *p++=*q; } *p=0;

	// Make sure auth'd
	unsigned idmsgcopilotnext = 3;
	msgcopilot(sprintfs("{\"params\":{},\"method\":\"checkStatus\",\"jsonrpc\":\"2.0\",\"id\":%ld}",idmsgcopilotnext++));
	while (1) {

		// Break if auth'd
		char *msg = getmsgcopilotid(idmsgcopilotnext-1); if (!msg) goto checkerrcopilot;
		char *result = getkjson(msg, "result");
		char *status = getkjson(result, "status");
		if (!strncmp(status,"\"OK\"",4)) break;
		if (strncmp(status,"\"NotSignedIn\"",13)) {
			char *message = getksjson(getkjson(msg,"error"), "message");
			if (message) {
				errcopilot=ECPAUTHSTR; if (!(serrcopilot=strdup(message))) { perror("cpwrap: copilot: strdup"); exit(1); }
				goto checkerrcopilot;
			} else {
				errcopilot=ECPAUTH;
				goto checkerrcopilot;
			}
		}

		// Do auth flow
		msgcopilot(sprintfs("{\"jsonrpc\":\"2.0\",\"params\":{},\"method\":\"signInInitiate\",\"id\":%d}",idmsgcopilotnext++));
		if (!(msg=getmsgcopilotid(idmsgcopilotnext-1))) goto checkerrcopilot;
		result = getkjson(msg, "result");
		char *usercode = getksjson(result, "userCode"); if (usercode && !(usercode=strdup(usercode))) { perror("cpwrap: strdup"); exit(1); }
		char *verificationuri = getksjson(result, "verificationUri");
		if (!usercode || !verificationuri) { free(usercode),errcopilot=ECPAUTHGETCODEURI; goto checkerrcopilot; }
		if ((res=pthread_mutex_lock(&muttranscript))) { fprintf(stderr,"cpwrap: pthread_mutex_lock transcript: %s\n",strerror(res)); exit(1); }
		writes(fdterm, sprintfs("%scpwrap: To authorize, enter this code at this uri:\n%s\n%s\n",ntranscript&&transcript[ntranscript-1]!='\n'?"\n":"",usercode,verificationuri));
		if ((res=pthread_mutex_unlock(&muttranscript))) { fprintf(stderr,"cpwrap: pthread_mutex_unlock transcript: %s\n",strerror(res)); exit(1); }
		msgcopilot(sprintfs("{\"jsonrpc\":\"2.0\",\"params\":{\"userCode\":\"%s\"},\"method\":\"signInConfirm\",\"id\":%d}",usercode,idmsgcopilotnext++));
		free(usercode);
	}

	// Start thread to write when transcript changes
	static unsigned long vtranscriptrequested = 0;
	int fdspolltranscript[2]; if (pipe(fdspolltranscript)) { perror("cpwrap: pipe"); exit(1); }
	struct writecond waittranscript = {fdspolltranscript[1], &muttranscript, &condtranscript, isvtranscriptdifferent, (void*)&vtranscriptrequested};
	pthread_t twaittranscript; if ((res=pthread_create(&twaittranscript,NULL,dowritecond,&waittranscript))) { fprintf(stderr,"cpwrap: pthread_create waitpoll: %s\n",strerror(res)); exit(1); }

	// Loop forever getting completions
	while (1) {
		startloopdocopilot:;

		// Wait for the transcript to change
		if ((res=pthread_mutex_lock(&muttranscript))) { fprintf(stderr,"cpwrap: pthread_mutex_lock transcript copilot0: %s\n",strerror(res)); exit(1); }
		while (1) {
			if (vtranscriptcompletion!=atomic_load(&vtranscript) && itranscriptcur==ntranscript && atomic_load(&ison)) break;
			if (verbose) fprintf(stderr,"\033[33mwaiting for transcript to change\033[0m\n");
			if ((res=pthread_cond_wait(&condtranscript, &muttranscript))) { fprintf(stderr,"cpwrap: pthread_cond_wait transcript copilot: %s\n",strerror(res)); exit(1); }
		}
		if (verbose || verbosetranscript) fprintf(stderr,"\033[33mtranscript (%zu):\n%.*s\033[0m\n", ntranscript, (int)ntranscript, transcript);

		// Get copy of transcript
		static size_t sizetranscriptcopy=0; size_t ntranscriptcopy=0; static char *transcriptcopy=NULL;
		if (sizetranscriptcopy<ntranscript && !(transcriptcopy=realloc(transcriptcopy,sizetranscriptcopy=ntranscript*2))) { fprintf(stderr,"cpwrap: realloc docopilot transcriptcopy %zu: %s\n",sizetranscriptcopy,strerror(errno)); exit(1); }
		memcpy(transcriptcopy, transcript, ntranscriptcopy=ntranscript);
		unsigned vtranscriptcopy = vtranscript;
		size_t icurcopy = itranscriptcur;
		if ((res=pthread_mutex_unlock(&muttranscript))) { fprintf(stderr,"cpwrap: pthread_mutex_unlock transcript copilot0: %s\n",strerror(res)); exit(1); }

		// Send reject/accept message from previous completion
		static char *uuid = NULL;
		if (uuid) {
			msgcopilot(acceptedcompletion
				? sprintfs("{\"params\":{\"uuid\":\"%s\"},\"id\":%u,\"method\":\"notifyAccepted\",\"jsonrpc\":\"2.0\"}",uuid,idmsgcopilotnext++)
				: sprintfs("{\"params\":{\"uuids\":[\"%s\"]},\"id\":%u,\"method\":\"notifyRejected\",\"jsonrpc\":\"2.0\"}",uuid,idmsgcopilotnext++));
			free(uuid), uuid=NULL;
		}

		// Request completion
		static size_t sizereq=0; size_t nreq=0; static char *req=NULL;
		unsigned iline=0,icharline=0; for (unsigned i=0; i<icurcopy; i++) { if (transcriptcopy[i]=='\n') iline++,icharline=0; else icharline++; }
		char *part; int npart;
		npart = strlen(part=sprintfs("{\"params\":{\"textDocument\":{\"uri\":\"file:\\/\\/%s\",\"languageId\":\"sh\",\"relativePath\":\"%1$s\"},\"doc\":{\"version\":0,\"languageId\":\"sh\",\"relativePath\":\"%1$s\",\"uri\":\"file:\\/\\/%1$s\",\"insertSpaces\":false,\"position\":{\"character\":%u,\"line\":%u},\"path\":\"%1$s\",\"indentSize\":2,\"tabSize\":2,\"source\":\"",pathdummyesc,icharline,iline));
		if (sizereq<nreq+npart && !(req=realloc(req,sizereq=(nreq+npart)*2))) { fprintf(stderr,"cpwrap: realloc docopilot req %zu: %s\n",sizereq,strerror(errno)); exit(1); }
		memcpy(req+nreq,part,npart), nreq+=npart;
		if (sizereq<nreq+(ntranscriptcopy*6) && !(req=realloc(req,sizereq=(nreq+(ntranscriptcopy*6))*2))) { fprintf(stderr,"cpwrap: realloc docopilot req %zu: %s\n",sizereq,strerror(errno)); exit(1); }
		for (char *p=transcriptcopy; p<transcriptcopy+ntranscriptcopy; p++) {
			const char *hex = "0123456789abcdef";
			if (*p == '\n') req[nreq++]='\\', req[nreq++]='n';
			else if (*p == '\r') req[nreq++]='\\', req[nreq++]='r';
			else if (*p == '\\') req[nreq++]='\\', req[nreq++]='\\';
			else if (*p == '"') req[nreq++]='\\', req[nreq++]='"';
			else if (*p == '\t') req[nreq++]='\\', req[nreq++]='t';
			else if (*p == '\b') req[nreq++]='\\', req[nreq++]='b';
			else if (*p == '\f') req[nreq++]='\\', req[nreq++]='f';
			else if (*p == '\a') req[nreq++]='\\', req[nreq++]='a';
			else if (*p == '\v') req[nreq++]='\\', req[nreq++]='v';
			else if (*p < 0x20) req[nreq++]='\\', req[nreq++]='u', req[nreq++]='0', req[nreq++]='0', req[nreq++]=hex[(*p>>4)&0xf], req[nreq++]=hex[*p&0xf];
			else req[nreq++] = *p;
			//TODO utf8
		}
		npart = strlen(part=sprintfs("\"},\"position\":{\"character\":%u,\"line\":%u}},\"id\":%u,\"method\":\"getCompletions\",\"jsonrpc\":\"2.0\"}",icharline,iline,idmsgcopilotnext++));
		if (sizereq<nreq+npart+1 && !(req=realloc(req,sizereq=(nreq+npart+1)*2))) { fprintf(stderr,"cpwrap: realloc docopilot req %zu: %s\n",sizereq,strerror(errno)); exit(1); }
		strcpy(req+nreq,part), nreq+=npart;
		msgcopilot(req);

		// Wait for completion until transcript changes
		if ((res=pthread_mutex_lock(&muttranscript))) { fprintf(stderr, "cpwrap: pthread_mutex_lock: %s\n", strerror(res)); exit(1); }
		waittranscript.want=1, vtranscriptrequested=vtranscriptcopy;
		if ((res=pthread_cond_broadcast(&condtranscript))) { fprintf(stderr, "cpwrap: pthread_cond_broadcast: %s\n", strerror(res)); exit(1); }
		if ((res=pthread_mutex_unlock(&muttranscript))) { fprintf(stderr, "cpwrap: pthread_mutex_unlock: %s\n", strerror(res)); exit(1); }
		char *msg; while (1) {
			if (atomic_load(&vtranscript) != vtranscriptrequested) goto startloopdocopilot;
			if (getmsgcopilotidnonblock(idmsgcopilotnext-1,&msg)) goto checkerrcopilot;
			if (msg) break;
			struct pollfd pfds[] = {{.fd=fdscp[2],.events=POLLIN}, {.fd=fdspolltranscript[0],.events=POLLIN}};
			if (verbose || verbosecopilot>=2) fprintf(stderr,"\33[33mwaiting for transcript/copilot\33[0m\n");
			if ((res=poll(pfds,sizeof(pfds)/sizeof(*pfds),-1)) == -1) { perror("cpwrap: poll"); exit(1); }
			char c; while (!willblockread(fdspolltranscript[0])) if (read(fdspolltranscript[0],&c,1)==-1 && errno!=EINTR) { perror("cpwrap: read polltranscript"); exit(1); }
		}

		// Parse completion response
		char *jcompletion = getijson(getkjson(getkjson(msg,"result"),"completions"), 0);
		size_t ncompletionnew=0; char *completionnew=NULL;
		free(uuid); if ((uuid=getksjson(jcompletion,"uuid"))) {
			if (!(uuid=strdup(uuid))) { perror("cpwrap: strdup uuid"); exit(1); }
			char *completionreq = getksjson(jcompletion, "displayText");
			if (completionreq) {
				if (!(completionnew=strdup(completionreq))) { fprintf(stderr,"cpwrap: strdup completionnew: %s\n",strerror(errno)); exit(1); }
				while (!memchr("\r\n",completionnew[ncompletionnew],3)) ncompletionnew++;
			}
		}

		// Set completion
		if ((res=pthread_mutex_lock(&mutcompletion))) { fprintf(stderr, "cpwrap: pthread_mutex_lock mutcompletion failed: %s\n", strerror(res)); exit(1); }
		free(completion), completion=completionnew;
		ncompletion = ncompletionnew;
		vtranscriptcompletion = vtranscriptcopy;
		acceptedcompletion = 0;
		writeall(fdscompletion[1], 1, "");
		if ((res=pthread_mutex_unlock(&mutcompletion))) { fprintf(stderr, "cpwrap: pthread_mutex_unlock mutcompletion failed: %s\n", strerror(res)); exit(1); }

		// Send shown message
		if (uuid) msgcopilot(sprintfs("{\"params\":{\"uuid\":\"%s\"},\"id\":%u,\"method\":\"notifyShown\",\"jsonrpc\":\"2.0\"}",uuid,idmsgcopilotnext++));

		// Check copilot errors
		checkerrcopilot:;
		if (atomic_load(&mainexited)) return NULL;
		if (errcopilot) {
			fprintf(stderr, "cpwrap: copilot error: ");
			if (errcopilot == ECPUNKNOWN) fprintf(stderr, "unknown error");
			else if (errcopilot == ECPAUTHSTR) fprintf(stderr, "error while authing: %s",serrcopilot), free(serrcopilot);
			else if (errcopilot == ECPAUTH) fprintf(stderr, "error while authing");
			else if (errcopilot == ECPAUTHGETCODEURI) fprintf(stderr, "error while getting the authentication userCode and verificationUri");
			else if (errcopilot == ECPREADEOF) fprintf(stderr, "EOF while reading");
			fprintf(stderr, "\n");
			errcopilot = 0;
			// TODO: restart and retry? meh
			atomic_store(&ison, 0);
			return NULL;
		}
	}
	return NULL;
}

int
main(int argc, char *argv[])
{
	int res;

	// Find copilot dist directory
	char *copilotdir=NULL, *sourcecpdir=NULL;
	if ((copilotdir=getenv("CPWRAPPATHDIST"))) { sourcecpdir="$CPWRAPPATHDIST"; goto gotdistdir; }
	char *xdgconfighomevar = getenv("XDG_CONFIG_HOME");
	char *home = getenv("HOME");
	char *xdgconfighome=xdgconfighomevar; if (!xdgconfighome && home && !(xdgconfighome=strdup(sprintfs("%s/.config",home)))) { perror("cpwrap: strdup xdgconfighome"); exit(1); }
	if (xdgconfighome && (copilotdir=readstr(sprintfs("%s/cpwrap/pathdist",xdgconfighome)))) { sourcecpdir="${XDG_CONFIG_HOME:$HOME/.config}/cpwrap/pathdist"; goto gotdistdir; }
	if ((copilotdir=(char*)pathcopilotdist)) { sourcecpdir="pathcopilotdist"; goto gotdistdir; }
	if (home) {
		char *s;
		if (!access(s=sprintfs("%s/nvim/pack/github/start/copilot.vim/copilot/dist",xdgconfighome),R_OK)) { if(!(copilotdir=strdup(s))){perror("cpwrap: strdup");exit(1);} sourcecpdir="${XDG_CONFIG_HOME:$HOME/.config}/nvim/pack/github/start/copilot.vim/copilot/dist"; goto gotdistdir; }
		if (!access(s=sprintfs("%s/.vim/pack/github/start/copilot.vim/copilot/dist",home),R_OK)) { if(!(copilotdir=strdup(s))){perror("cpwrap: strdup");exit(1);} sourcecpdir="~/.vim/pack/github/start/copilot.vim/copilot/dist"; goto gotdistdir; }
	}
	//TODO: windows?
	printf("Please enter the absolute path to the copilot extension 'dist' directory. If you have installed the copilot vim/nvim extension, it will be wherever that extension is stored. Or you can get the folder from here:\nhttps://github.com/github/copilot.vim/tree/release/copilot/dist\nand enter its path on your system.\nPath: "); fflush(stdout);
	while (1) {
		size_t sizel=0; if (getline(&copilotdir,&sizel,stdin) == -1) { perror("cpwrap: getline"); exit(1); }
		if (!*copilotdir) return 0;
		char *pnl=strchr(copilotdir,'\n'); if (pnl)*pnl='\0';
		if (*copilotdir != '/') printf("Please enter an absolute path (starting with '/')");
		else if (!access(copilotdir,F_OK) && !access(sprintfs("%s/agent.js",copilotdir),R_OK)) break;
		else printf("access '%s/agent.js': %s",copilotdir,strerror(errno));
		printf("\nPath: "); fflush(stdout);
	}
	sourcecpdir = "user input";
	gotdistdir:;
	if (access(copilotdir,F_OK)) { fprintf(stderr, "cpwrap: couldn't access copilot dist directory '%s' gotten from %s: %s\n", copilotdir, sourcecpdir, strerror(errno)); exit(1); }
	if (access(sprintfs("%s/agent.js",copilotdir),R_OK)) { fprintf(stderr, "cpwrap: couldn't access agent.js file in dist directory '%s' gotten from %s (are you sure that is the dist directory?): %s\n", copilotdir, sourcecpdir, strerror(errno)); exit(1); }
	if (home && !strcmp(sourcecpdir,"user input")) mkdir(sprintfs("%s/cpwrap",xdgconfighome),0755), writestr(sprintfs("%s/cpwrap/pathdist",xdgconfighome),copilotdir), printf("Saved to %s/cpwrap/pathdist\n",xdgconfighome);

	// Start copilot
	if (pipe(fdscp) || pipe(fdscp+2)) { perror("cpwrap: pipe fdscp"); exit(1); }
	if (fcntl(fdscp[0],F_SETFL,FD_CLOEXEC)==-1 || fcntl(fdscp[1],F_SETFL,FD_CLOEXEC)==-1 || fcntl(fdscp[2],F_SETFL,FD_CLOEXEC)==-1 || fcntl(fdscp[3],F_SETFL,FD_CLOEXEC)==-1) { perror("cpwrap: fcntl fdscp"); exit(1); }
	if ((pidcp=fork()) == -1) { perror("cpwrap: fork"); exit(1); }
	if (!pidcp) {
		if (dup2(fdscp[0],0)==-1 || dup2(fdscp[3],1)==-1) { perror("cpwrap: dup2 fdscp"); exit(1); }
		if (!verbose && !verbosecopilot) close(2);
		if (execlp("node","node",sprintfs("%s/agent.js",copilotdir),NULL) == -1) { perror("cpwrap: execlp node (make sure node.js is installed)"); exit(1); }
	}
	close(fdscp[0]), close(fdscp[3]);

	// Fork args as pseudoterminal
	int fdmaster = posix_openpt(O_RDWR); if (fdmaster == -1) { perror("cpwrap: posix_openpt"); exit(1); }
	if (grantpt(fdmaster) == -1) { perror("cpwrap: grantpt"); exit(1); }
	if (unlockpt(fdmaster) == -1) { perror("cpwrap: unlockpt"); exit(1); }
	char *pathslave = ptsname(fdmaster); if (!pathslave) { perror("cpwrap: ptsname"); exit(1); }
	int fdslave = open(pathslave, O_RDWR); if (fdslave == -1) { perror("cpwrap: open"); exit(1); }
	if (fcntl(fdmaster,F_SETFL,FD_CLOEXEC)==-1 || fcntl(fdslave,F_SETFL,FD_CLOEXEC)==-1) { perror("cpwrap: fcntl fdmaster fdslave"); exit(1); }
	pid_t pid = fork(); if (pid == -1) { perror("cpwrap: fork"); exit(1); }
	if (!pid) {
		if (setsid() == -1) { perror("cpwrap: setsid"); exit(1); }
		if (ioctl(fdslave,TIOCSCTTY,0) == -1) { perror("cpwrap: ioctl"); exit(1); }
		if (dup2(fdslave,0)==-1 || dup2(fdslave,1)==-1 || dup2(fdslave,2)==-1) { perror("cpwrap: dup2"); exit(1); }
		if (execvp(argv[1], argv+1)) { perror("cpwrap: execvp"); exit(1); }
	}
	close(fdslave);

	// Open terminal for writing
	fdterm = open("/dev/tty", O_WRONLY|O_CLOEXEC); if (fdterm == -1) { perror("cpwrap: open /dev/tty"); exit(1); }

	// Clean up in exit
	if (atexit(cleanup)) { perror("cpwrap: atexit"); exit(1); }

	// Set terminal to raw mode
	if (tcgetattr(fdterm,&oldtermios) == -1) { perror("cpwrap: tcgetattr"); exit(1); }
	gottermios = 1;
	struct termios newtermios = oldtermios;
	newtermios.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	if (tcsetattr(fdterm,TCSANOW,&newtermios)) { perror("cpwrap: tcsetattr"); exit(1); }

	// Buffers for in-|>process-|>out
	size_t sizefromstdin=0, nfromstdin=0; char *fromstdin=NULL;
	size_t sizefromprocess=0, nfromprocess=0; char *fromprocess=NULL;
	if (!(fromstdin=malloc(sizefromstdin=2048)) || !(fromprocess=malloc(sizefromprocess=2048))) { perror("cpwrap: malloc"); exit(1); }

	// Create completion pipe
	if (pipe(fdscompletion)) { perror("cpwrap: pipe fdscompletion"); exit(1); }
	if (fcntl(fdscompletion[0],F_SETFL,FD_CLOEXEC)==-1 || fcntl(fdscompletion[1],F_SETFL,FD_CLOEXEC)==-1) { perror("cpwrap: fcntl fdscompletion"); exit(1); }

	// Start transcript off with args
	int nsargs=3; for (int i=1; i<argc; i++) nsargs+=strlen(argv[i])+1;
	if (!(transcript=malloc(sizetranscript=nsargs<2048?2048:nsargs*2))) { perror("cpwrap: malloc"); exit(1); }
	ntranscript=0; for (int i=1; i<argc; i++) {
		if (i > 1) transcript[ntranscript++]=' ';
		strcpy(transcript+ntranscript,argv[i]), ntranscript+=strlen(argv[i]);
	}

	// Start copilot thread
	pthread_t tcopilot; if (pthread_create(&tcopilot,NULL,docopilot,NULL)) { perror("cpwrap: pthread_create docopilot"); exit(1); }

	// Say help
	writes(fdterm, "cpwrap: TAB=accept, ESC=reject, F1=toggle\n");

	// Loop forever piping between
	char closedprocess=0, visiblecompletion=0, visiblestatus=0;
	while (!closedprocess || nfromprocess) {

		// Wait for anything to be actionable
		struct pollfd pfds[] = {
			{0, POLLIN},
			{closedprocess?-1:fdmaster, POLLIN|(nfromstdin?POLLOUT:0)},
			{1, nfromprocess?POLLOUT:0},
			{fdscompletion[0], POLLIN}
		};
		if (verbose) fprintf(stderr, "waiting on poll (%zu %zu)...\n", nfromstdin, nfromprocess);
		int mspoll = atomic_load(&ison)&&vtranscriptcompletion!=atomic_load(&vtranscript) ? 100 : -1;
		if (poll(pfds,sizeof(pfds)/sizeof(*pfds),mspoll)==-1 && errno!=EAGAIN && errno!=EINTR) { perror("cpwrap: poll"); exit(1); }
		if (verbose) fprintf(stderr, "poll returned: %d %d %d %d (POLLIN=%d, POLLOUT=%d, POLLERR=%d, POLLHUP=%d)\n", pfds[0].revents, pfds[1].revents, pfds[2].revents, pfds[3].revents, POLLIN, POLLOUT, POLLERR, POLLHUP);
		if ((res=pthread_mutex_lock(&muttranscript))) { fprintf(stderr, "cpwrap: pthread_mutex_lock transcript main: %d\n", res); exit(1); }
		unsigned vtranscript0 = atomic_load(&vtranscript);

		// Read all of stdin
		size_t nfromstdin0 = nfromstdin;
		while (!willblockread(0)) {
			if (sizefromstdin<nfromstdin+2048 && !(fromstdin=realloc(fromstdin,sizefromstdin*=2))) { perror("cpwrap: realloc stdin"); exit(1); }
			ssize_t n = read(0, fromstdin+nfromstdin, sizefromstdin-nfromstdin); if (n==-1 && errno!=EAGAIN && errno!=EINTR) { perror("cpwrap: read stdin"); exit(1); }
			if (verbose) fprintf(stderr, "read %zd bytes from stdin (now %zu)\n", n, nfromstdin+n);
			//const char *hex = "0123456789abcdef"; for (int i=0; i<n; i++) fprintf(stderr,"%c%c", hex[(fromstdin[nfromstdin+i]>>4)&0xf], hex[fromstdin[nfromstdin+i]&0xf]); fputc('\n', stderr);
			if (!n) exit(0);
			if (n > 0) nfromstdin += n;
		}
		//if (nfromstdin > nfromstdin0) { const char*hex="0123456789abcdef"; fprintf(stderr,"\n\033[32mread from stdin (%zu):\n", nfromstdin-nfromstdin0); for (size_t i=nfromstdin0; i<nfromstdin; i++) { char c=fromstdin[i]; if (c=='\n') writes(2,"\\n"); else if (c=='\r') writes(2,"\\r"); else if (c=='\t') writes(2,"\\t"); else if (c=='\033') writes(2,"\\33"); else if (c<32) { writes(2,"\\x"); writeall(2,1,hex+(c>>4)); writeall(2,1,hex+(c&15)); } else writeall(2,1,&c); } writes(2,"\033[0m\n"); }

		// Accept completion
		if (vtranscriptcompletionshown==atomic_load(&vtranscript) && visiblecompletion && nfromstdin-nfromstdin0==1 && fromstdin[nfromstdin0]=='\t') {
			writes(fdterm, "\033[K");
			visiblecompletion=0, acceptedcompletion=1;
			if (sizefromstdin<nfromstdin-1+ncompletion && !(fromstdin=realloc(fromstdin,sizefromstdin=(nfromstdin-1+ncompletion)*2))) { perror("cpwrap: realloc fromstdin"); exit(1); }
			memcpy(fromstdin+nfromstdin0,completion,ncompletion), nfromstdin=nfromstdin0+ncompletion;
		}

		// Reject completion
		if (vtranscriptcompletionshown==atomic_load(&vtranscript) && visiblecompletion && nfromstdin-nfromstdin0==1 && fromstdin[nfromstdin0]=='\33') {
			writes(fdterm, "\033[K");
			visiblecompletion = 0;
			nfromstdin = nfromstdin0;
		}

		// Toggle on or off
		if (nfromstdin-nfromstdin0==3 && !memcmp(fromstdin+nfromstdin0,"\33OP",3)) {
			atomic_store(&ison, !atomic_load(&ison));
			if (visiblecompletion || visiblestatus) writes(fdterm,"\033[K"), visiblecompletion=visiblestatus=0;
			nfromstdin = nfromstdin0;
		}

		// Write all to process
		while (nfromstdin && !willblockwrite(fdmaster)) {
			ssize_t n = write(fdmaster, fromstdin, nfromstdin); if (n==-1 && errno!=EAGAIN && errno!=EINTR) { perror("cpwrap: write process stdin"); exit(1); }
			if (!n) exit(0);
			if (n > 0) memmove(fromstdin,fromstdin+n,nfromstdin-=n);
		}

		// Read from process
		size_t nfromprocess0 = nfromprocess;
		while (!willblockread(fdmaster)) {
			if (sizefromprocess<nfromprocess+2048 && !(fromprocess=realloc(fromprocess,sizefromprocess*=2))) { perror("cpwrap: realloc process stdout"); exit(1); }
			ssize_t n = read(fdmaster, fromprocess+nfromprocess, sizefromprocess-nfromprocess); if (n==-1 && errno!=EAGAIN && errno!=EINTR && errno!=EIO) { perror("cpwrap: read process stdout"); exit(1); }
			if (verbose) fprintf(stderr, "read %zd bytes from process (now %zu)\n", n, n>0?nfromprocess+n:nfromprocess);
			if (n==-1 && errno==EIO) { closedprocess=1; break; }
			if (!n) { closedprocess=1; break; }
			if (n > 0) nfromprocess += n;
		}
		if (sizefromprocess<nfromprocess+1 && !(fromprocess=realloc(fromprocess,sizefromprocess*=2))) { perror("cpwrap: realloc process stdout"); exit(1); }
		fromprocess[nfromprocess] = 0;
		//if (nfromprocess > nfromprocess0) { const char*hex="0123456789abcdef"; fprintf(stderr,"\n\033[32mread from process (%zu):\n", nfromprocess-nfromprocess0); for (size_t i=nfromprocess0; i<nfromprocess; i++) { char c=fromprocess[i]; if (c=='\n') writes(2,"\\n"); else if (c=='\r') writes(2,"\\r"); else if (c=='\t') writes(2,"\\t"); else if (c=='\033') writes(2,"\\33"); else if (c<32) { writes(2,"\\x"); writeall(2,1,hex+(c>>4)); writeall(2,1,hex+(c&15)); } else writeall(2,1,&c); } writes(2,"\033[0m\n"); }

		// Append to output portion of transcript, processing terminal escape codes
		size_t nadd = nfromprocess - nfromprocess0;
		if (sizetranscript<ntranscript+nadd && !(transcript=realloc(transcript,sizetranscript=(ntranscript+nadd)*2))) { perror("cpwrap: realloc transcript"); exit(1); }
		//TODO utf8 / finding actual terminal width of characters
		for (size_t i=nfromprocess0; i<nfromprocess;) {
			size_t nleft = nfromprocess - i;
			if (0);
			#define ELIF(s) else if (nleft>=sizeof(s)-1 && !memcmp(fromprocess+i,s,sizeof(s)-1) && (i+=sizeof(s)-1,1))
			ELIF("\33[C") { if (itranscriptcur++ == ntranscript) transcript[ntranscript]=' '; }
			ELIF("\33[K") ntranscript=itranscriptcur;
			ELIF("\33[") {
				char *pend; unsigned long n = strtoul(fromprocess+i, &pend, 10);
				if (pend == fromprocess+i) n=-1;
				i = pend - fromprocess + (pend<fromprocess+nfromprocess);
				if (*pend == '@') {
					if (n == -1) n=1;
					if (sizetranscript<itranscriptcur+n && !(transcript=realloc(transcript,sizetranscript=(itranscriptcur+n)*2))) { perror("cpwrap: realloc transcript @"); exit(1); }
					memmove(transcript+itranscriptcur+n, transcript+itranscriptcur, ntranscript-itranscriptcur);
					memset(transcript+itranscriptcur, ' ', n);
				} else if (*pend == 'P') {
					if (n == -1) n=1;
					if (itranscriptcur < ntranscript) {
						if (ntranscript-itranscriptcur < n) n=ntranscript-itranscriptcur;
						memmove(transcript+itranscriptcur,transcript+itranscriptcur+n,ntranscript-itranscriptcur-n), ntranscript-=n;
					}
				}
			} ELIF("\7");
			ELIF("\10") { if (itranscriptcur && transcript[itranscriptcur-1]!='\n') itranscriptcur--; }
			ELIF("\r") while (itranscriptcur && transcript[itranscriptcur-1]!='\n') itranscriptcur--;
			ELIF("\n") transcript[ntranscript]='\n', itranscriptcur=ntranscript+1;
			#undef ELIF
			else transcript[itranscriptcur++]=fromprocess[i++];
			if (itranscriptcur > ntranscript) ntranscript=itranscriptcur;
		}
		if (itranscriptcur < ntranscript) {
			for (size_t i=itranscriptcur; i<ntranscript; i++) if (transcript[i]!=' ') goto notruncatetranscripttocur;
			ntranscript = itranscriptcur;
		} notruncatetranscripttocur:;
		if (nadd) atomic_store(&vtranscript, atomic_load(&vtranscript)+1);
		//fprintf(stderr,"\n\33[32mtranscript after processing (%zu)\n",ntranscript); const char*hex="0123456789abcdef"; for (size_t i=0; i<=ntranscript; i++) { if (i==itranscriptcur)writes(2,"|"); if (i==ntranscript)break; char c=transcript[i]; if (c=='\n') writes(2,"\n"); else if (c=='\r') writes(2,"\\r"); else if (c=='\t') writes(2,"\\t"); else if (c=='\033') writes(2,"\\33"); else if (c<32) { writes(2,"\\x"); writeall(2,1,hex+(c>>4)); writeall(2,1,hex+(c&15)); } else writeall(2,1,&c); } writes(2,"\033[0m\n");

		// Release transcript
		if (atomic_load(&vtranscript)!=vtranscript0 && ((res=pthread_cond_broadcast(&condtranscript)))) { fprintf(stderr, "cpwrap: pthread_cond_broadcast transcript main: %d\n", res); exit(1); }
		if ((res=pthread_mutex_unlock(&muttranscript))) { fprintf(stderr, "cpwrap: pthread_mutex_unlock transcript main: %d\n", res); exit(1); }

		// Write all to stdout
		while (nfromprocess && !willblockwrite(1)) {
			if (visiblecompletion || visiblestatus) writes(fdterm,"\33[K"), visiblecompletion=visiblestatus=0;
			ssize_t n = write(1, fromprocess, nfromprocess); if (n==-1 && errno!=EAGAIN && errno!=EINTR) { perror("cpwrap: write stdout"); exit(1); }
			if (!n) exit(0);
			if (n > 0) memmove(fromprocess,fromprocess+n,nfromprocess-=n);
		}

		// Draw completion status
		if ((res=pthread_mutex_lock(&mutcompletion))) { fprintf(stderr, "cpwrap: pthread_mutex_lock completion main: %s\n",strerror(res)); exit(1); }
		char tmp; while (!willblockread(fdscompletion[0])) { ssize_t r=read(fdscompletion[0],&tmp,sizeof(tmp)); if (r==-1 && errno!=EAGAIN && errno!=EINTR) { perror("cpwrap: read completion"); exit(1); } if (!r) { fputs("cpwrap: unexpected EOF on completion pipe\n",stderr); exit(1); } }
		unsigned msnow = getms();
		if (atomic_load(&ison) && itranscriptcur==ntranscript) {
			if (atomic_load(&vtranscript)==vtranscriptcompletion && vtranscriptcompletionshown!=vtranscriptcompletion) {
				if (visiblecompletion || visiblestatus) writes(fdterm,"\33[K"), visiblecompletion=visiblestatus=0;
				if (ncompletion) {
					writes(fdterm, "\0337\033[4m");
					writeall(fdterm, ncompletion, completion);
					writes(fdterm, "\033[24m\033[K\0338");
					visiblecompletion = 1;
				}
				vtranscriptcompletionshown = vtranscriptcompletion;
			} else if (atomic_load(&vtranscript) != vtranscriptcompletion) {
				if (visiblecompletion || visiblestatus) writes(fdterm,"\33[K"), visiblecompletion=visiblestatus=0;
				writes(fdterm, "\0337\33[4m");
				writeall(fdterm, 1, ". "+(msnow/100)%2);
				writes(fdterm, "\33[24m\0338");
				visiblestatus = 1;
			}
		}
		if ((res=pthread_mutex_unlock(&mutcompletion))) { fprintf(stderr, "cpwrap: pthread_mutex_unlock completion main: %s\n",strerror(res)); exit(1); }
	}
	if (visiblestatus || visiblecompletion) writes(fdterm,"\33[K");
	atomic_store(&mainexited, 1);
}
