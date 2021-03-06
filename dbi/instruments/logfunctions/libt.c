/*
 *  Collin's Binary Instrumentation Tool/Framework for Android
 *  Collin Mulliner <collin[at]mulliner.org>
 *  http://www.mulliner.org/android/
 *
 *  (c) 2012
 *
 *  License: GPL v2
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <string.h>
#include <termios.h>

#include "util.h"
#include "libt.h"


// contains macro of my_init()
// autogenerated
#include "hijacks.h"

// communication socket, a pty :)
int coms;

void __attribute__ ((constructor)) my_init(void);

void my_cacheflush(unsigned int begin, unsigned int end)
{	
	const int syscall = 0xf0002;
	__asm __volatile (
		"mov	 r0, %0\n"			
		"mov	 r1, %1\n"
		"mov	 r7, %2\n"
		"mov     r2, #0x0\n"
		"svc     0x00000000\n"
		:
		:	"r" (begin), "r" (end), "r" (syscall)
		:	"r0", "r1", "r7"
		);
}


/*
  
  hook_t holds hook metadata 
  hookf is the hook callback
  
 */
int hook(struct hook_t *h, int pid, char *libname, char *funcname, void *hookf)
{
	unsigned long int addr;
	int i;
	int find_name_result;

	// 1 - fetch function address and put it into addr
	find_name_result = find_name(pid, funcname, libname, &addr);
	if ( find_name_result < 0) {
	  log("can't find: %s - error code %d\n", funcname, find_name_result)
		return 0;
	}
	//	addr = 0x8361;
	
	log("hooking   %s = %x  hook = %x  target:", funcname, addr, hookf)
	strncpy(h->name, funcname, sizeof(h->name)-1);
	

	// 2 - hook either ARM or THUMB code
	if (addr % 4 == 0) {
		log("ARM\n")
		h->thumb = 0;
		h->patch = (unsigned int)hookf;
		h->orig = addr;
		h->jump[0] = 0xe59ff000; // LDR pc, [pc, #0]
		h->jump[1] = h->patch;
		h->jump[2] = h->patch;
		for (i = 0; i < 3; i++)
			h->store[i] = ((int*)h->orig)[i];
		for (i = 0; i < 3; i++)
			((int*)h->orig)[i] = h->jump[i];
	}
	else {
		h->thumb = 1;
		log("THUMB\n")
		h->patch = (unsigned int)hookf;
		h->orig = addr;	
		h->jumpt[1] = 0xb4;
		h->jumpt[0] = 0x30; // push {r4,r5}
		h->jumpt[3] = 0xa5;
		h->jumpt[2] = 0x03; // add r5, pc, #12
		h->jumpt[5] = 0x68;
		h->jumpt[4] = 0x2d; // ldr r5, [r5]
		h->jumpt[7] = 0xb0;
		h->jumpt[6] = 0x02; // add sp,sp,#8
		h->jumpt[9] = 0xb4;
		h->jumpt[8] = 0x20; // push {r5}
		h->jumpt[11] = 0xb0;
		h->jumpt[10] = 0x81; // sub sp,sp,#4
		h->jumpt[13] = 0xbd;
		h->jumpt[12] = 0x20; // pop {r5, pc}
		h->jumpt[15] = 0x46;
		h->jumpt[14] = 0xaf; // mov pc, r5 ; just to pad to 4 byte boundary


		// h->patch
		memcpy(&h->jumpt[16], (unsigned char*)&h->patch, sizeof(unsigned int));

		unsigned int orig = addr - 1; // sub 1 to get real address

		for (i = 0; i < 20; i++) {
			h->storet[i] = ((unsigned char*)orig)[i];
			log("%0.2x ", h->storet[i])
		}

		log("\n")
		for (i = 0; i < 20; i++) {
			((unsigned char*)orig)[i] = h->jumpt[i];
			log("%0.2x ", ((unsigned char*)orig)[i])
		}
		log("\n")
	}

	my_cacheflush((unsigned int)h->orig, (unsigned int)h->orig+12);
	return 1;
}

void hook_precall(struct hook_t *h)
{
  log("precall enter\n")

	int i;
	
	if (h->thumb) {
		unsigned int orig = h->orig - 1;
		for (i = 0; i < 12; i++) {
			((unsigned char*)orig)[i] = h->storet[i];
		}
	}
	else {
		for (i = 0; i < 3; i++)
			((int*)h->orig)[i] = h->store[i];
	}	
  log("precall bail\n");
	
  my_cacheflush((unsigned int)h->orig, (unsigned int)h->orig+12);
	
}

void hook_postcall(struct hook_t *h)
{
	int i;
	
	if (h->thumb) {
		unsigned int orig = h->orig - 1;
		for (i = 0; i < 12; i++)
			((unsigned char*)orig)[i] = h->jumpt[i];
	}
	else {
		for (i = 0; i < 3; i++)
			((int*)h->orig)[i] = h->jump[i];
	}
	my_cacheflush((unsigned int)h->orig, (unsigned int)h->orig+12);	
}

void unhook(struct hook_t *h)
{
	log("unhooking %s = %x  hook = %x ", h->name, h->orig, h->patch)
	hook_precall(h);
}

/*
 *  workaround for blocked socket API when process does not have network
 *  permissions
 *
 *  this code simply opens a pseudo terminal (pty) which gives us a
 *  file descriptor. the pty then can be used by another process to
 *  communicate with our instrumentation code. an example program
 *  would be a simple socket-to-pty-bridge
 *  
 *  this function just creates and configures the pty
 *  communication (read, write, poll/select) has to be implemented by hand
 *
 */
void start_coms()
{
	// coms is a global
	coms = open("/dev/ptmx", O_RDWR|O_NOCTTY);
	if (coms <= 0)
		log("posix_openpt failed\n")
	//else
	//	log("pty created\n")
	if (unlockpt(coms) < 0)
		log("unlockpt failed\n")
	log("pty created, file name: %s\n", ptsname(coms))
	
	struct termios  ios;
	tcgetattr(coms, &ios);
	ios.c_lflag = 0;  // disable ECHO, ICANON, etc...
	tcsetattr(coms, TCSANOW, &ios);
}


void my_init()
{
  log("libt loaded...\n")
	
    //start_coms();
    
  int pid = getpid();
  
  hook(&hook_BMalloc, pid, "libpolarisoffice", "BMalloc", my_BMalloc);
	
  log("libt _init done.\n")
}
