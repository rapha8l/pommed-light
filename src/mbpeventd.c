/*
 * $Id$
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <poll.h>
#include <signal.h>

#include <errno.h>

#include <smbios/SystemInfo.h>

#include "mbpeventd.h"
#include "kbd_backlight.h"
#include "lcd_backlight.h"
#include "cd_eject.h"
#include "evdev.h"


/* Machine type */
int machine;

/* Used by signal handlers */
static int running;


int
check_machine_smbios(void)
{
  int ret = -1;

  const char *prop;

  if (geteuid() != 0)
    {
      debug("Error: SMBIOS machine detection only works as root\n");

      return ret;
    }

  /* Check vendor name */
  prop = SMBIOSGetVendorName();

  debug("SMBIOS vendor name: [%s]\n", prop);
  if (strcmp(prop, "Apple Computer, Inc.") == 0)
    ret = MACHINE_VENDOR_APPLE;

  SMBIOSFreeMemory(prop);

  if (ret != MACHINE_VENDOR_APPLE)
    return ret;

  ret = MACHINE_MAC_UNKNOWN;

  /* Check system name */
  prop = SMBIOSGetSystemName();

  debug("SMBIOS system name: [%s]\n", prop);
  if (strcmp(prop, "MacBookPro2,2") == 0)
    ret = MACHINE_MACBOOKPRO_22;

  SMBIOSFreeMemory(prop);

  return ret;
}


void
sig_int_term_handler(int signal)
{
  running = 0;
}

int
main (int argc, char **argv)
{
  int ret;
  int i;

  FILE *pidfile;

  struct pollfd *fds;
  int nfds;

  struct timeval tv_now;
  struct timeval tv_als;
  struct timeval tv_diff;

  machine = check_machine_smbios();
  switch (machine)
    {
      case MACHINE_MACBOOKPRO_22:
	debug("Detected (SMBIOS) a MacBookPro2,2\n");
	break;
      case MACHINE_MAC_UNKNOWN:
	fprintf(stderr, "Error: unknown Apple machine\n");

	exit(1);
	break;
      default:
	fprintf(stderr, "Error: unknown non-Apple machine\n");

	exit(1);
	break;
    }


  ret = lcd_backlight_probe_X1600();
  if (ret < 0)
    {
      fprintf(stderr, "Error: no Radeon Mobility X1600 found\n");

      exit(1);
    }

  nfds = open_evdev(&fds);
  if (nfds < 1)
    {
      fprintf(stderr, "Error: no devices found\n");

      exit(1);
    }

  kbd_backlight_status_init();

  /*
   * Detach from the console
   */
  if (daemon(1, 1) != 0)
    {
      fprintf(stderr, "Error: daemon() failed: %s\n", strerror(errno));

      close_evdev(&fds, nfds);
      exit(-1);
    }

  pidfile = fopen(PIDFILE, "w");
  if (pidfile == NULL)
    {
      debug("Could not open pidfile: %s\n", strerror(errno));

      close_evdev(&fds, nfds);
      exit(-1);
    }
  fprintf(pidfile, "%d\n", getpid());
  fclose(pidfile);

  gettimeofday(&tv_als, NULL);

  running = 1;
  signal(SIGINT, sig_int_term_handler);
  signal(SIGTERM, sig_int_term_handler);

  while (1 && running)
    {
      ret = poll(fds, nfds, LOOP_TIMEOUT);

      if (ret < 0) /* error */
	{
	  debug("Poll error: %s\n", strerror(errno));

	  break;
	}
      else
	{
	  if (ret != 0)
	    {
	      for (i = 0; i < nfds; i++)
		{
		  if (fds[i].revents & POLLIN)
		    process_evdev_events(fds[i].fd);
		}
	    }

	  /* only check ambient light sensors when poll() times out
	   * or when we haven't checked since LOOP_TIMEOUT
	   */
	  gettimeofday(&tv_now, NULL);
	  if (ret == 0)
	    {
	      kbd_backlight_ambient_check();
	      tv_als = tv_now;
	    }
	  else
	    {
	      tv_diff.tv_sec = tv_now.tv_sec - tv_als.tv_sec;
	      if (tv_diff.tv_sec < 0)
		tv_diff.tv_sec = 0;

	      if (tv_diff.tv_sec == 0)
		{
		  tv_diff.tv_usec = tv_now.tv_usec - tv_als.tv_usec;
		}
	      else
		{
		  tv_diff.tv_sec--;
		  tv_diff.tv_usec = 1000000 - tv_als.tv_usec + tv_now.tv_usec;
		  tv_diff.tv_usec += tv_diff.tv_sec * 1000000;
		}

	      if (tv_diff.tv_usec >= (1000 * LOOP_TIMEOUT))
		{
		  kbd_backlight_ambient_check();
		  tv_als = tv_now;
		}
	    }
	}
    }

  close_evdev(&fds, nfds);
  unlink(PIDFILE);

  return 0;
}
