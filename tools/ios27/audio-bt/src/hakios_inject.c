/*
 * hakios-inject LIBRARY PROGRAM [ARGS...]: run PROGRAM with LIBRARY inserted (DYLD_INSERT_LIBRARIES), for a launchd job
 * whose ProgramArguments start with it. launchd drops DYLD_ variables from job descriptions, and the kernel's boot-arg
 * sweetinject (which also makes dyld accept the variable) has no room left on the S9's 608-byte command line; exec keeps
 * the job's pid and bootstrap port, so launchd and the program's Mach services see no difference. Needs sweetinject= set
 * (any value) so that AMFI's dyld policy lets the variable through. Scion, APSL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: hakios-inject LIBRARY PROGRAM [ARGS...]\n");
		return 64;
	}
	setenv("DYLD_INSERT_LIBRARIES", argv[1], 1);
	execv(argv[2], argv + 2);
	perror(argv[2]);
	return 71;
}
