/* nxgipd.c
 * 
 * Copyright (C) 2009-2015 Timo Kokkonen. All Rights Reserved.
 *
 * $Id$
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#if HAVE_GETOPT_H && HAVE_GETOPT_LONG
#include <getopt.h>
#else
#include "getopt.h"
#endif


#define DEBUG 0

#include "nx-584.h"
#include "nxgipd.h"

int verbose_mode = 0;

nx_shm_t *shm = NULL;
int shmid = -1;
int msgid = -1;
nx_interface_status_t *istatus;
nx_system_status_t *astat;
nx_configuration_t configuration;
nx_configuration_t *config = &configuration;



void crash_signal_handler(int sig)
{
  logmsg(0,"program crashed: signal=%d (%s)", sig, strsignal(sig));

  if (shm != NULL) 
    release_shared_memory(shmid,shm);
  if (msgid >= 0) 
    release_message_queue(msgid);

  _exit(2); // avoid running any at_exit functions
}


void signal_handler(int sig)
{
  logmsg(0,"program terminated: signal=%d (%s)", sig, strsignal(sig));
  exit(1);
}


void exit_cleanup()
{
  logmsg(3,"exit_cleanup()");
  if (config->status_file && astat) {
    int r = save_status_xml(config->status_file, astat);
    if (r != 0) {
      logmsg(0,"failed to save alarm status: %s (%d)",config->status_file,r);
    }
  }

  if (shm != NULL) 
    release_shared_memory(shmid,shm);
  if (msgid >= 0) 
    release_message_queue(msgid);
}



int main(int argc, char **argv)
{
  int fd;
  nxmsg_t msgin,msgout;
  int ret, retry;
  int opt_index = 0;
  int opt;
  int scan_mode = 0;
  int scan_node = 0;
  int scan_loc = -1;
  int log_mode = 0;
  int daemon_mode = 0;
  char *config_file = CONFIG_FILE;
  char *pid_file = NULL;
  struct sigaction sigact;
  struct option long_options[] = {
    {"config",1,0,'c'},
    {"daemon",0,0,'d'},
    {"help",0,0,'h'},
    {"log",0,0,'l'},
    {"log-only",0,0,'L'},
    {"pid",1,0,'p'},
    {"probe",0,0,'P'},
    {"scan",1,0,'s'},
    {"status",0,0,'S'},
    {"verbose",0,0,'v'},
    {"version",0,0,'V'},
    {NULL,0,0,0}
  };
  nx_ipc_msg_t  ipcmsg;

  config->syslog_mode=0;
  config->debug_mode=0;

  umask(022);

  while ((opt=getopt_long(argc,argv,"vVhc:dp:l",long_options,&opt_index)) != -1) {
    switch (opt) {
      
    case 'c':
      config_file=strdup(optarg);
      break;

    case 'p':
      pid_file=strdup(optarg);
      break;

    case 'd':
      daemon_mode=1;
      break;

    case 'v':
      verbose_mode=1;
      break;

    case 'V':
      fprintf(stderr,"%s v%s (%s)  %s\nCopyright (C) 2009-2015 Timo Kokkonen. All Rights Reserved.\n",
	      PRGNAME,VERSION,BUILDDATE,HOST_TYPE);
      exit(0);
      
    case 's':
      if (sscanf(optarg,"%d,%d",&scan_node,&scan_loc)==2) {
	scan_mode=1;
	if (scan_loc < 0 || scan_loc > 1024) die("invalid location number: %d",scan_loc);
      }
      else if (sscanf(optarg,"%d",&scan_node)==1) {
	scan_mode=1;
	scan_loc=-1;
      }
      if (scan_node < 0 || scan_node > 255) die("invalid module number: %d",scan_node);
      break;

    case 'S':
      scan_mode=2;
      break;

    case 'P':
      scan_mode=3;
      break;

    case 'l':
      log_mode=1;
      break;

    case 'L':
      log_mode=2;
      break;
      
    case 'h':
    default:
      fprintf(stderr,"Usage: %s [OPTIONS]\n\n",PRGNAME);
      fprintf(stderr,
	      "  --config=<configfile>, -c <configfile>\n"
	      "                          use specified config file\n"
	      "  --pid=<pidfile>, -p <pidfile>\n"
	      "                          save process pid in file\n"
	      "  --daemon, -d            run as background daemon process\n"
	      "  --help, -h              display this help and exit\n"
	      "  --log, -l               dump panel log when starting\n"
	      "  --log-only              dump panel log and exit\n"
	      "  --probe                 probe bus for modules and exit\n"
	      "  --scan=<module>         dump full config of a module and exit\n"
	      "  --scan=<xmodule>,<loc>   dump single config location of a module and exit\n"
	      "  --status                display NX gateway status/settings\n"
	      "  --verbose, -v           enable verbose output to stdout\n"
	      "  --version, -V           print program version\n"
	      "\n");
      exit(1);
    }
  }


  printf("Loading configuration...\n");
  if (load_config(config_file,config,1))
    die("Failed to open configuration file: %s",config_file);

  if (config->status_file && verbose_mode) {
    printf("Using alarm status file: %s\n",config->status_file);
  }



  /* setup signal handling */
  sigact.sa_handler=signal_handler;
  sigemptyset(&sigact.sa_mask);
  sigact.sa_flags=0;
  sigaction(SIGTERM,&sigact,NULL);
  sigaction(SIGINT,&sigact,NULL);
  sigact.sa_handler=crash_signal_handler;
  sigaction(SIGSEGV,&sigact,NULL);
  sigaction(SIGBUS,&sigact,NULL);
  sigact.sa_handler=SIG_IGN;
  sigaction(SIGHUP,&sigact,NULL);


  atexit(exit_cleanup);


  /* initialize IPC shared memory segment */
  if (init_shared_memory(config->shmkey,config->shmmode,sizeof(nx_shm_t),&shmid,&shm))
    die("Failed to initialize IPC shared memory segment");
  istatus=&shm->intstatus;
  astat=&shm->alarmstatus;
  if (verbose_mode) 
    printf("IPC shm: key=0x%08x id=%d",config->shmkey,shmid);

  /* initialize IPC message queue */
  if ((msgid=init_message_queue(config->msgkey,config->msgmode)) < 0) 
    die("Failed to initialize IPC message queue");
  if (verbose_mode)
    printf("IPC msg: key=0x%08x id=%d",config->msgkey,msgid); 



  printf("Opening serial port: %s\n",config->serial_device);
  if ((fd = openserialdevice(config->serial_device,config->serial_speed)) < 0)
    die("Failed to open serial port");


  printf("Establishing communications...\n");

  /* clear anypending messages */
  do {
    ret=nx_receive_message(fd,config->serial_protocol,&msgin,1);
  } while (ret==1);

  /* request interface configuration */
  msgout.msgnum=NX_INT_CONFIG_REQ;
  msgout.len=1;
  retry=0;
  do {
    ret=nx_send_message(fd,config->serial_protocol,&msgout,5,3,NX_INT_CONFIG_MSG,&msgin);
    if (ret < 0) logmsg(0,"Failed to send message\n");
    if (ret == 0) logmsg(0,"No response from NX device\n");
  } while (ret != 1 && retry++ < 3);
  if (ret == 1) {
    if (msgin.msgnum != NX_INT_CONFIG_MSG) die("NX device refused the command\n");
    if (scan_mode==2) {
      nx_print_msg(stdout,&msgin);
      exit(0);
    }
  } else {
    die("failed to estabilish communications with NX device");
  }
  process_message(&msgin,1,0,astat,istatus);
  printf("Interface version v%s detected\n",istatus->version);


  if (scan_mode > 0) {
    /* scan of device or bus requested */

    if ( (istatus->sup_cmd_msgs[2] & 0x01) == 0 ) 
      die("Program Data Request command not enabled.");

    if (scan_mode==1) {
      read_config(fd,config->serial_protocol,scan_node,scan_loc);
      exit(0);
    }
    else if (scan_mode==3) {
      probe_bus(fd,config->serial_protocol);
      exit(0);
    }
  }




  /* spawn daemon */
  if (daemon_mode) {
    int fd;
    pid_t pid;

    if (chdir("/") < 0) die("cannot access root directory");

    pid = fork();
    if (pid < 0) die("fork() failed");
    if (pid > 0) {
      printf("Daemon started (pid=%d)...\n",pid);
      _exit(0); /* avoid running atexit functions... */
    }
    
    setsid();
    
    if ((fd = open("/dev/null",O_RDWR)) < 0) die("cannot open /dev/null");
    dup2(fd,0);
    dup2(fd,1);
    dup2(fd,2);
    close(fd);
  }

  /* create pid file */
  shm->pid=getpid();
  if (pid_file) {
    FILE *fp = fopen(pid_file,"w");
    if (!fp) die("failed to create pid file: %s",pid_file);
    fprintf(fp,"%d\n",shm->pid);
    fclose(fp);
  }


  logmsg(0,"Program started: %s v%s (%s)",PRGNAME,VERSION,BUILDDATE);
  logmsg(1,"NX interface version v%s detected",istatus->version);

  if (log_mode > 0) {
    printf("Dumping panel log...");
    dump_log(fd,config->serial_protocol,astat,istatus);
    if (log_mode == 2) exit(0);
  }


  logmsg(0,"Getting system status...");
  retry=0;
  while (1) {
    ret=get_system_status(fd,config->serial_protocol,astat,istatus);
    printf("\n");
    if (ret < 0) {
      printf("failed to get system status: %d\n",ret);
      if (retry++ >= 3) die("communication problem, giving up");
      sleep(1);
    } else { 
      break;
    }
  }


  logmsg(0,"Waiting for messages");
  shm->daemon_started=time(NULL);
  shm->last_updated=time(NULL);

  /* main process loop */
  while (1) {

    /* wait for message to come in (or timeout)... */
    ret=nx_receive_message(fd,config->serial_protocol,&msgin,3);
    if (ret < -1) {
      logmsg(0,"error reading message");
    } else if (ret == -1) {
      logmsg(0,"invalid message received");
    } else if (ret == 1) {
      if (verbose_mode) printf("got message %02x!\n",msgin.msgnum & NX_MSG_MASK);
      //logmsg(3,"got message %02x",msgin.msgnum & NX_MSG_MASK);
      process_message(&msgin,0,verbose_mode,astat,istatus);
      if ((msgin.msgnum & NX_MSG_MASK) == NX_KEYPAD_MSG_RCVD) {
	//firmware bug panel ignores ACK for this message...
      }
    } else {
      /* no message received (timeout), check if there is anything else to do... */

      time_t t = time(NULL);


      /* periodially check that panel is responding... */
      if (astat->last_statuscheck + (astat->statuscheck_interval*60) < t) {
	msgout.msgnum=NX_SYS_STATUS_REQ;
	msgout.len=1;
	ret=nx_send_message(fd,config->serial_protocol,&msgout,5,3,NX_SYS_STATUS_MSG,&msgin);
	if (ret == 1 && msgin.msgnum == NX_SYS_STATUS_MSG) {
	  process_message(&msgin,0,verbose_mode,astat,istatus);
	  logmsg(1,"panel ok");
	  shm->comm_fail=0;
	} else {
	  logmsg(0,"failure to communicate with panel!");
	  shm->comm_fail=1;
	}
	astat->last_statuscheck=t;
      }


      /* update panel clock, if its time... */
      if ( (astat->timesync_interval > 0) && 
	   (astat->last_timesync + (astat->timesync_interval*3600) < t) ) {
	process_set_clock(fd,config->serial_protocol,astat);
      }


      /* check for messages in message queue */
      if ((ret=read_message_queue(msgid,&ipcmsg)) > 0) {
	logmsg(3,"got IPC message: %d (%02x,%02x,%02x,...) = %d",
	       ipcmsg.msgtype,ipcmsg.data[0],ipcmsg.data[1],ipcmsg.data[2],ret);
	
	switch (ipcmsg.msgtype) {
	case NX_IPC_MSG_CMD:
	  process_command(fd,config->serial_protocol,ipcmsg.data,istatus);
	  break;
	case NX_IPC_MSG_BYPASS:
	  process_zone_bypass_command(fd,config->serial_protocol,ipcmsg.data,istatus);
	  break;
	case NX_IPC_MSG_GET_PROG:
	  process_get_program_command(fd,config->serial_protocol,ipcmsg.data,istatus);
	  break;
	case NX_IPC_MSG_MESSAGE:
	  process_keypadmsg_command(fd,config->serial_protocol,ipcmsg.data,istatus);
	  break;
	default:
	  logmsg(0,"unknown IPC message received: %d",ipcmsg.msgtype);
	}
	
	memset(ipcmsg.data,0,sizeof(ipcmsg.data)); // clear message data so PIN won't be left in memory
      }

    }

    fflush(stdout);
    shm->last_updated=time(NULL);
  }


  close(fd);
  exit(0);
}

