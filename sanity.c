#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

#define PGSIZE 4096
#define COUNT 20

char* m1[COUNT];

volatile int
main(int argc, char *argv[])
{
	int i, pid, proc_pid, wpid;
	char* address;

	//creating 'COUNT' pages
	for (i = 0; i < COUNT; ++i)
	{
		if ((pid = fork()) == 0){ // son
			address = sbrk(PGSIZE);
			proc_pid = getpid();
			printf(1, "proc_pid=%d allocated page #%d at address: %x\n", proc_pid, i, address);
			exit();
		}
	}
	
	while((wpid=wait()) >= 0){
		printf(1, "exit pid=%d\n", wpid);
	}
	printf(1,"Finished Successfuly!!!\n");

	exit();
	return 0;
}

/*
volatile int
main(int argc, char *argv[])
{
	int i,j;

	//creating 'COUNT' pages
	for (i = 0; i < COUNT; ++i)
	{
		m1[i] = sbrk(PGSIZE);
		printf(1, "allocated page #%d at address: %x\n", i, m1[i]);
	}

	//using all pages
	for ( i = 0; i < COUNT; ++i)
	{
		printf(1, "init page #%d with zero\n", i);
		for ( j = 0; j < PGSIZE; ++j)
		{
			m1[i][j] = 0;
		}
	}

	printf(1,"Finished Successfuly!!!\n");

	exit();
	return 0;
}*/