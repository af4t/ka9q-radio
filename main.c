// Core of KA9Q radiod
// downconvert, filter, demodulate, multicast output
// Copyright 2017-2025, Phil Karn, KA9Q, karn@ka9q.net
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <iniparser/iniparser.h>
#include <net/if.h>
#include <sched.h>
#include <sysexits.h>
#include <fcntl.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "rtp.h"
#include "radio.h"
#include "filter.h"
#include "status.h"
#include "config.h"
#include "avahi.h"

// Configuration constants & defaults
static char const DEFAULT_PRESET[] = "am";
static int const DEFAULT_FFTW_THREADS = 1;
static int const DEFAULT_IP_TOS = 48; // AF12 left shifted 2 bits
static float const DEFAULT_BLOCKTIME = 20.0;
static int const DEFAULT_OVERLAP = 5;
static int const DEFAULT_UPDATE = 25; // 2 Hz for 20 ms blocktime (50 Hz frame rate)
static int const DEFAULT_LIFETIME = 20; // 20 sec for idle sessions tuned to 0 Hz
#define GLOBAL "global"

char const *Description; // Set either in [global] or [hardware]
char const *Iface;
char *Data;
char const *Preset = DEFAULT_PRESET;
char Preset_file[PATH_MAX];
char const *Config_file;
char Hostname[256]; // can't use sysconf(_SC_HOST_NAME_MAX) at file scope

int IP_tos = DEFAULT_IP_TOS;
float Blocktime = DEFAULT_BLOCKTIME;
int Overlap = DEFAULT_OVERLAP;
static int Update = DEFAULT_UPDATE;
static int RTCP_enable = false;
static int SAP_enable = false;
extern bool Affinity;

// List of valid config keys in [global] section, for error checking
char const *Global_keys[] = {
  "affinity",
  "blocktime",
  "data",
  "description",
  "dns",
  "fft-plan-level",
  "fft-threads",
  "fft-time-limit",
  "hardware",
  "iface",
  "mode-file",
  "mode",
  "overlap",
  "preset",
  "presets-file",
  "prio",
  "rtcp",
  "sap",
  "static",
  "status",
  "tos",
  "ttl",
  "update",
  "verbose",
  "wisdom-file",
  NULL
};


// Command line and environ params
const char *App_path;
int Verbose;
static char const *Locale = "en_US.UTF-8";

static dictionary *Configtable; // Configtable file descriptor for iniparser for main radiod config file
dictionary *Preset_table;   // Table of presets, usually in /usr/local/share/ka9q-radio/presets.conf
volatile bool Stop_transfers = false; // Request to stop data transfers; how should this get set?

static int64_t Starttime;      // System clock at timestamp 0, for RTCP
static pthread_t Status_thread;
static char *Metadata_dest_string; // DNS name of default multicast group for status/commands
int Output_fd = -1; // Unconnected socket used for output when ttl > 0
int Output_fd0 = -1; // Unconnected socket used for local loopback when ttl = 0
struct channel Template; // Template for dynamically created channels
// If a channel is tuned to 0 Hz and then not polled for this many seconds, destroy it
// Must be computed at run time because it depends on the block time
int Channel_idle_timeout;  //  = DEFAULT_LIFETIME * 1000 / Blocktime;
int Ctl_fd = -1;     // File descriptor for receiving user commands
static char const *Name;
extern int N_worker_threads; // owned by filter.c
static void *Dl_handle;
static bool Global_use_dns;
static int Nchans;

static void closedown(int);
static void verbosity(int);
static int loadconfig(char const *file);
static int setup_hardware(char const *sname);
static void *rtcp_send(void *);
void *process_section(void *p);

// In sdrplay.c (maybe someday)
int sdrplay_setup(struct frontend *,dictionary *,char const *);
int sdrplay_startup(struct frontend *);
double sdrplay_tune(struct frontend *,double);

// In rx888.c
int rx888_setup(struct frontend *,dictionary *,char const *);
int rx888_startup(struct frontend *);
double rx888_tune(struct frontend *,double);
float rx888_gain(struct frontend *, float);
float rx888_atten(struct frontend *,float);

// In airspy.c
int airspy_setup(struct frontend *,dictionary *,char const *);
int airspy_startup(struct frontend *);
double airspy_tune(struct frontend *,double);

// In airspyhf.c
int airspyhf_setup(struct frontend *,dictionary *,char const *);
int airspyhf_startup(struct frontend *);
double airspyhf_tune(struct frontend *,double);

// In funcube.c
int funcube_setup(struct frontend *,dictionary *,char const *);
int funcube_startup(struct frontend *);
double funcube_tune(struct frontend *,double);

// In rtlsdr.c:
int rtlsdr_setup(struct frontend *,dictionary *,char const *);
int rtlsdr_startup(struct frontend *);
double rtlsdr_tune(struct frontend *,double);

// In sig_gen.c:
int sig_gen_setup(struct frontend *,dictionary *,char const *);
int sig_gen_startup(struct frontend *);
double sig_gen_tune(struct frontend *,double);



// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  App_path = argv[0];

  VERSION();
#ifndef NDEBUG
  fprintf(stderr,"Assertion checking enabled, execution will be slower\n");
#endif

  setlinebuf(stderr);
  Starttime = gps_time_ns();

  struct timespec start_realtime;
  clock_gettime(CLOCK_MONOTONIC,&start_realtime);


  // Set up program defaults
  // Some can be overridden by command line args
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists

  int c;
  while((c = getopt(argc,argv,"N:hvp:V")) != -1){
    switch(c){
    case 'V': // Already shown above
      exit(EX_OK);
    case 'p':
      FFTW_plan_timelimit = strtod(optarg,NULL);
      break;
    case 'v':
      Verbose++;
      break;
    case 'N':
      Name = optarg;
      break;
    default: // including 'h'
      fprintf(stderr,"Unknown command line option %c\n",c);
      fprintf(stderr,"Usage: %s [-I] [-N name] [-h] [-p fftw_plan_time_limit] [-v [-v] ...] <CONFIG_FILE>\n", argv[0]);
      exit(EX_USAGE);
    }
  }

  // Graceful signal catch
  signal(SIGINT,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);
  signal(SIGUSR1,verbosity);
  signal(SIGUSR2,verbosity);

  if(argc <= optind){
    fprintf(stderr,"Configtable file missing\n");
    exit(EX_NOINPUT);
  }
  Config_file = argv[optind];
  if(Name == NULL){
    // Extract name from config file pathname
    Name = argv[optind]; // Ah, just use whole thing
  }

  int const n = loadconfig(Config_file);
  if(n < 0){
    fprintf(stderr,"Can't load config file %s\n",Config_file);
    exit(EX_NOINPUT);
  }
  fprintf(stderr,"%d total demodulators started\n",n);
  if(Ctl_fd == -1 && n == 0){
    fprintf(stderr,"Warning: no control channel and no static demodulators, radiod won't do anything\n");
  }

  // Measure CPU usage
  int sleep_period = 60;
  struct timespec last_realtime = start_realtime;
  struct timespec last_cputime = {0};
  while(true){
    sleep(sleep_period);
    if(Verbose){
      struct timespec new_realtime;
      clock_gettime(CLOCK_MONOTONIC,&new_realtime);
      double total_real = new_realtime.tv_sec - start_realtime.tv_sec
	+ 1e-9 * (new_realtime.tv_nsec - start_realtime.tv_nsec);

      struct timespec new_cputime;
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&new_cputime);
      double total_cpu = new_cputime.tv_sec + 1e-9 * (new_cputime.tv_nsec);

      double total_percent = 100. * total_cpu / total_real;

      double period_real = new_realtime.tv_sec - last_realtime.tv_sec
	+ 1e-9 * (new_realtime.tv_nsec - last_realtime.tv_nsec);

      double period_cpu =  new_cputime.tv_sec - last_cputime.tv_sec
	+ 1e-9 * (new_cputime.tv_nsec - last_cputime.tv_nsec);

      double period_percent = 100. * period_cpu / period_real;

      last_realtime = new_realtime;
      last_cputime = new_cputime;
      fprintf(stderr,"CPU usage: %.1lf%% since start, %.1lf%% in last %.1lf sec\n",
	      total_percent, period_percent,period_real);
    }
  }
  exit(EX_OK); // Can't happen
}

// Load the radiod config file, e.g., /etc/radio/radiod@rx888-ka9q-hf.conf
static int loadconfig(char const *file){
  if(file == NULL || strlen(file) == 0)
    return -1;

  DIR *dirp = NULL;
  struct stat statbuf = {0};
  char dname[PATH_MAX] = {0};
  if(stat(file,&statbuf) == 0){
    switch(statbuf.st_mode & S_IFMT){
    case S_IFREG:
      // primary config file radiod@foo.conf exists and is a regular file; just read it
      fprintf(stderr,"Loading config file %s\n",file);
      Configtable = iniparser_load(file); // Just try to read the primary
      if(Configtable == NULL)
	return -1;
      break;
    case S_IFDIR:
      // It's a directory, read its contents
      fprintf(stderr,"Loading config directory %s\n",file);
      dirp = opendir(file);
      if(dirp == NULL)
	return -1; // give up
      break;
    default:
      fprintf(stderr,"Config file %s exists but is not a regular file or directory\n",file);
      return -1;
    }
  } else {
    // Otherwise append ".d" and see if that's a directory
    snprintf(dname,sizeof(dname),"%s.d",file);
    if(stat(dname,&statbuf) == 0 && (statbuf.st_mode & S_IFMT) == S_IFDIR){
      fprintf(stderr,"Loading config directory %s\n",dname);
      dirp = opendir(dname);
    }
  }
  if(Configtable == NULL){
    if(dirp == NULL){
      fprintf(stderr,"%s Not a valid config file/directory\n",file);
      return -1; // give up
    }
    // Read and sort list of foo.d/*.conf files, merge into temp file
    int dfd = dirfd(dirp); // this gets used for openat() and fstatat() so don't close dirp right way
    struct dirent *dp;
    int const n_subfiles = 100;
    char *subfiles[n_subfiles]; // List of subfiles
    int sf = 0;
    while ((dp = readdir(dirp)) != NULL && sf < n_subfiles) {
      // only consider regular files ending in .conf
      if(strcmp(".conf",dp->d_name + strlen(dp->d_name) - 5) == 0
	 && fstatat(dfd,dp->d_name,&statbuf,0) == 0
	 && (statbuf.st_mode & S_IFMT) == S_IFREG)
	subfiles[sf++] = strdup(dp->d_name);
    }
    if(sf == 0){
      fprintf(stderr,"%s: empty config directory\n",strlen(dname) > 0 ? dname : file);
      closedir(dirp);
      return -1;
    }
    // Don't close dirp just yet, would invalidate dfd
    // Config sections can actually be in any order, but just in case one is split across multiple files...
    qsort(subfiles,sf,sizeof(subfiles[0]),(int (*)(void const *,void const *))strcmp);

    // Concatenate sorted files into temporary copy
    char tempfilename[PATH_MAX];
    strlcpy(tempfilename,"/tmp/radiod-configXXXXXXXX",sizeof(tempfilename));
    int tfd = mkstemp(tempfilename);
    if(tfd == -1){
      fprintf(stderr,"mkstemp(%s) failed: %s\n",tempfilename,strerror(errno));
      closedir(dirp);
      return -1;
    }
    FILE *tfp = fdopen(tfd,"rw+");
    if(tfp == NULL){
      fprintf(stderr,"Can't create temporary file %s: %s\n",tempfilename,strerror(errno));
      close(tfd);
      (void)closedir(dirp);
      return -1;
    }
    // Concatenate the sub config files in order
    for(int i=0; i < sf; i++){
      int fd = openat(dfd,subfiles[i],O_RDONLY|O_CLOEXEC);
      // There's no "fopenat()"
      if(fd == -1){
	fprintf(stderr,"Can't read config component %s: %s\n",subfiles[i],strerror(errno));
	continue;
      }
      FILE *fp = fdopen(fd,"r");
      if(fp == NULL){
	fprintf(stderr,"fdopen(%d,r) of %s failed: %s\n",fd,subfiles[i],strerror(errno));
	close(fd);
	continue;
      }
      fprintf(tfp,"# %s\n",subfiles[i]); // for debugging
      char *linep = NULL;
      size_t linecap = 0;
      while(getline(&linep,&linecap,fp) >= 0){
	if(fputs(linep,tfp) == EOF){
	  fprintf(stderr,"fputs(%s,%s): %s\n",linep,tempfilename,strerror(errno));
	  break;
	}
      }
      FREE(linep);
      fclose(fp);     fp = NULL;
    }
    // Done with file names and directory
    for(int i=0; i < sf; i++)
      FREE(subfiles[i]); // Allocated by strdup()

    (void)closedir(dirp); dirp = NULL;
    fclose(tfp); tfp = NULL; tfd = -1; // Also does close(tfd)

    Configtable = iniparser_load(tempfilename);
    unlink(tempfilename); // Done with temp file
  }
  if(Configtable == NULL){
    return -1;
  }
  config_validate_section(stderr,Configtable,GLOBAL,Global_keys,Channel_keys);

  // Process [global] section applying to all demodulator blocks
  Description = config_getstring(Configtable,GLOBAL,"description",NULL);
  Verbose = config_getint(Configtable,GLOBAL,"verbose",Verbose);
  Blocktime = fabs(config_getdouble(Configtable,GLOBAL,"blocktime",Blocktime));
  Channel_idle_timeout = 20 * 1000 / Blocktime;
  Overlap = abs(config_getint(Configtable,GLOBAL,"overlap",Overlap));
  N_worker_threads = config_getint(Configtable,GLOBAL,"fft-threads",DEFAULT_FFTW_THREADS); // variable owned by filter.c
  FFTW_plan_timelimit = config_getdouble(Configtable,GLOBAL,"fft-time-limit",FFTW_plan_timelimit);
  RTCP_enable = config_getboolean(Configtable,GLOBAL,"rtcp",RTCP_enable);
  SAP_enable = config_getboolean(Configtable,GLOBAL,"sap",SAP_enable);
  {
    char const *cp = config_getstring(Configtable,GLOBAL,"fft-plan-level","patient");
    if(strcasecmp(cp,"estimate") == 0){
      FFTW_planning_level = FFTW_ESTIMATE;
    } else if(strcasecmp(cp,"measure") == 0){
      FFTW_planning_level = FFTW_MEASURE;
    } else if(strcasecmp(cp,"patient") == 0){
      FFTW_planning_level = FFTW_PATIENT;
    } else if(strcasecmp(cp,"exhaustive") == 0){
      FFTW_planning_level = FFTW_EXHAUSTIVE;
    } else if(strcasecmp(cp,"wisdom-only") == 0){
      FFTW_planning_level = FFTW_WISDOM_ONLY;
    }
  }
  Update = config_getint(Configtable,GLOBAL,"update",Update);
  IP_tos = config_getint(Configtable,GLOBAL,"tos",IP_tos);
  Global_use_dns = config_getboolean(Configtable,GLOBAL,"dns",false);
  Static_avahi = config_getboolean(Configtable,GLOBAL,"static",false);
  Affinity = config_getboolean(Configtable,GLOBAL,"affinity",false);
  {
    char const *p = config_getstring(Configtable,GLOBAL,"wisdom-file",NULL);
    if(p != NULL)
      Wisdom_file = strdup(p);

    // Accept either keyword; "preset" is more descriptive than the old (but still accepted) "mode"
    p = config_getstring(Configtable,GLOBAL,"mode-file","presets.conf");
    p = config_getstring(Configtable,GLOBAL,"presets-file",p);
    dist_path(Preset_file,sizeof(Preset_file),p);
    fprintf(stderr,"Loading presets file %s\n",Preset_file);
    Preset_table = iniparser_load(Preset_file); // Kept open for duration of program
    config_validate(stderr,Preset_table,Channel_keys,NULL);
    if(Preset_table == NULL){
      fprintf(stderr,"Can't load preset file %s\n",Preset_file);
      exit(EX_UNAVAILABLE); // Can't really continue without fixing
    }
  }

  // Form default status dns name

  gethostname(Hostname,sizeof(Hostname));
  // Edit off .domain, .local, etc
  {
    char *cp = strchr(Hostname,'.');
    if(cp != NULL)
      *cp = '\0';
  }
  char default_status[strlen(Hostname) + strlen(Name) + 20]; // Enough room for snprintf
  snprintf(default_status,sizeof(default_status),"%s-%s.local",Hostname,Name);
  {
   char const *cp = config_getstring(Configtable,GLOBAL,"status",default_status); // Status/command target for all demodulators
    // Add .local if not present
   Metadata_dest_string = ensure_suffix(cp,".local");
  }
  // Set up the hardware early, in case it fails
  const char *hardware = config_getstring(Configtable,GLOBAL,"hardware",NULL);
  if(hardware == NULL){
    // 'hardware =' now required, no default
    fprintf(stderr,"'hardware = [sectionname]' now required to specify front end configuration\n");
    exit(EX_USAGE);
  }
  // Look for specified hardware section
  {
    int const nsect = iniparser_getnsec(Configtable);
    int sect;
    for(sect = 0; sect < nsect; sect++){
      char const * const sname = iniparser_getsecname(Configtable,sect);
      if(strcasecmp(sname,hardware) == 0){
	if(setup_hardware(sname) != 0)
	  exit(EX_NOINPUT);

	break;
      }
    }
    if(sect == nsect){
      fprintf(stderr,"no hardware section [%s] found, please create it\n",hardware);
      exit(EX_USAGE);
    }
  }
  if(strlen(Frontend.description) == 0)
    strlcpy(Frontend.description,Name,sizeof(Frontend.description)); // Set default description

  // Default multicast interface
  {
    // The area pointed to by returns from config_getstring() is freed and overwritten when the config dictionary is closed
    char const *p = config_getstring(Configtable,GLOBAL,"iface",Iface);
    if(p != NULL){
      Iface = strdup(p);
      Default_mcast_iface = Iface;
    }
  }
  // Overrides in [global] of compiled-in defaults
  {
    char data_default[256];
    snprintf(data_default,sizeof(data_default),"%s-pcm.local",Name);
    char const *cp = config_getstring(Configtable,GLOBAL,"data",data_default);
    // Add .local if not present
    Data = ensure_suffix(cp,".local");
  }
  // Set up template for all new channels
  set_defaults(&Template);
  Template.lifetime = DEFAULT_LIFETIME * 1000 / Blocktime; // If freq == 0, goes away 20 sec after last command

  // Set up default output stream file descriptor and socket
  // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
  strlcpy(Template.output.dest_string,Data,sizeof(Template.output.dest_string));

  // Preset/mode must be specified to create a dynamic channel
  // (Trying to switch from term "mode" to term "preset" as more descriptive)
  char const * p = config_getstring(Configtable,GLOBAL,"preset","am"); // Hopefully "am" is defined in presets.conf
  char const * preset = config_getstring(Configtable,GLOBAL,"mode",p); // Must be specified to create a dynamic channel
  if(preset != NULL){
    if(loadpreset(&Template,Preset_table,preset) != 0)
      fprintf(stderr,"warning: loadpreset(%s,%s) in [global]\n",Preset_file,preset);
    strlcpy(Template.preset,preset,sizeof(Template.preset));

    loadpreset(&Template,Configtable,GLOBAL); // Overwrite with other entries from this section, without overwriting those
  } else {
    fprintf(stderr,"No default mode for template\n");
  }

  /* The ttl in the [global] section is used for any dynamic
     data channels. It is the default for static data channels unless
     overridden in each section. Note that when a section specifies a
     non-zero TTL, the global setting is actually used so the section TTLs could as well be booleans.
     It's too tedious and not very useful to manage a whole bunch of sockets with arbitrary
     TTLs. 0 and 1 are most useful.
     At the moment, elicited status messages are always sent with TTL > 0 on the status group
  */

  // Look quickly (2 tries max) to see if it's already in the DNS
  {
    uint32_t addr = 0;
    if(!Global_use_dns || resolve_mcast(Data,&Template.output.dest_socket,DEFAULT_RTP_PORT,NULL,0,2) != 0)
      addr = make_maddr(Data);

    char ttlmsg[128];
    snprintf(ttlmsg,sizeof(ttlmsg),"TTL=%d",Template.output.ttl);
    size_t slen = sizeof(Template.output.dest_socket);
    // Advertise dynamic service(s)
    avahi_start(Frontend.description,
	      "_rtp._udp",
	      DEFAULT_RTP_PORT,
	      Data,
	      addr,
	      ttlmsg,
	      addr != 0 ? &Template.output.dest_socket : NULL,
	      addr != 0 ? &slen : NULL);

    // Status sent to same group, different port
    Template.status.dest_socket = Template.output.dest_socket;
    setport(&Template.status.dest_socket,DEFAULT_STAT_PORT);
   }
  {
    // Non-zero TTL streams use the global ttl if it is nonzero, 1 otherwise
    int const ttl = Template.output.ttl > 1 ? Template.output.ttl : 1;

    Output_fd = output_mcast(&Template.output.dest_socket,Iface,ttl,IP_tos);
    if(Output_fd < 0){
      fprintf(stderr,"can't create output socket for TTL=%d: %s\n",ttl,strerror(errno));
      exit(EX_NOHOST); // let systemd restart us
    }
  }
  join_group(Output_fd,NULL,&Template.output.dest_socket,Iface); // Work around snooping switch problem
  // Secondary output socket with ttl = 0
  Output_fd0 = output_mcast(&Template.output.dest_socket,Iface,0,IP_tos);
  if(Output_fd0 < 0){
    fprintf(stderr,"can't create output socket for TTL=0: %s\n",strerror(errno));
    exit(EX_NOHOST); // let systemd restart us
  }

  // Set up status/command stream, global for all receiver channels
  if(0 == strcmp(Metadata_dest_string,Data)){
    fprintf(stderr,"Duplicate status/data stream names: data=%s, status=%s\n",Data,Metadata_dest_string);
    exit(EX_USAGE);
  }
  // Look quickly (2 tries max) to see if it's already in the DNS
  {
    uint32_t addr = 0;
    if(!Global_use_dns || resolve_mcast(Metadata_dest_string,&Frontend.metadata_dest_socket,DEFAULT_STAT_PORT,NULL,0,2) != 0)
      addr = make_maddr(Metadata_dest_string);

    // If dns name already exists in the DNS, advertise the service record but not an address record
    // Advertise control/status channel with a ttl of at least 1
    char ttlmsg[128];
    snprintf(ttlmsg,sizeof ttlmsg,"TTL=%d",Template.output.ttl > 0? Template.output.ttl : 1);
    size_t slen = sizeof(Frontend.metadata_dest_socket);
    avahi_start(Frontend.description,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,
		Metadata_dest_string,addr,ttlmsg,
		addr != 0 ? &Frontend.metadata_dest_socket : NULL,
		addr != 0 ? &slen : NULL);
  }
  // either resolve_mcast() or avahi_start() has resolved the target DNS name into Frontend.metadata_dest_socket and inserted the port number
  join_group(Output_fd,NULL,&Frontend.metadata_dest_socket,Iface);
  // Same remote socket as status
  Ctl_fd = listen_mcast(NULL,&Frontend.metadata_dest_socket,Iface);
  if(Ctl_fd < 0){
    fprintf(stderr,"can't listen for commands from %s: %s; no control channel is set\n",Metadata_dest_string,strerror(errno));
  } else {
    if(Ctl_fd >= 3)
      pthread_create(&Status_thread,NULL,radio_status,NULL);
  }

  // Process individual demodulator sections in parallel for speed
  int const nsect = iniparser_getnsec(Configtable);
  pthread_t startup_threads[nsect];
  int nthreads = 0;
  for(int sect = 0; sect < nsect; sect++){
    char const * const sname = iniparser_getsecname(Configtable,sect);

    if(strcasecmp(sname,GLOBAL) == 0)
      continue; // Already processed above
    if(strcasecmp(sname,hardware) == 0)
      continue; // Already processed as a hardware section (possibly without device=)
    if(config_getstring(Configtable,sname,"device",NULL) != NULL)
      continue; // It's a front end configuration, ignore
    if(config_getboolean(Configtable,sname,"disable",false))
      continue; // section is disabled

    pthread_create(&startup_threads[nthreads++],NULL,process_section,(void *)sname);
  }
  // Wait for them all to start
  for(int sect = 0; sect < nthreads; sect++){
    pthread_join(startup_threads[sect],NULL);
#if 0
    printf("startup thread %s joined\n",iniparser_getsecname(Configtable,sect));
#endif
  }
  iniparser_freedict(Configtable);
  Configtable = NULL;
  return Nchans;
}
void *process_section(void *p){
  char const *sname = (char *)p;
  if(sname == NULL)
    return NULL;

  config_validate_section(stderr,Configtable,sname,Channel_keys,NULL);

  // fall back to setting in [global] if parameter not specified in individual section
  // Set parameters even when unused for the current demodulator in case the demod is changed later
  char const * preset = config2_getstring(Configtable,Configtable,GLOBAL,sname,"mode",NULL);
  preset = config2_getstring(Configtable,Configtable,GLOBAL,sname,"preset",preset);
  if(preset == NULL || strlen(preset) == 0)
    fprintf(stderr,"[%s] preset/mode not specified, all parameters must be explicitly set\n",sname);

  // Override [global] settings with section settings
  char *data = NULL;
  {
    char const *cp = config_getstring(Configtable,sname,"data",Data);
    // Add .local if not present
    data = ensure_suffix(cp,".local");
  }

  // Set up a template for all channels defined in this section
  // Parameter priority, from high to low:
  // 1. this section
  // 2. the preset database entry, if specified
  // 3. the [global] section
  // 4. compiled-in defaults to keep things from blowing up
  struct channel chan_template = {0};
  set_defaults(&chan_template); // compiled-in defaults (#4)
  loadpreset(&chan_template,Configtable,GLOBAL); // [global] section (#3)

  if(preset != NULL && loadpreset(&chan_template,Preset_table,preset) != 0) // preset database entry (#2)
    fprintf(stderr,"[%s] loadpreset(%s,%s) failed; compiled-in defaults and local settings used\n",sname,Preset_file,preset);

  strlcpy(chan_template.preset,preset,sizeof chan_template.preset);
  loadpreset(&chan_template,Configtable,sname); // this section's config (#1)

  if(chan_template.output.ttl != 0 && Template.output.ttl != 0)
    chan_template.output.ttl = Template.output.ttl; // use global ttl when both are non-zero

  // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
  // Look quickly (2 tries max) to see if it's already in the DNS. Otherwise make a multicast address.
  uint32_t addr = 0;
  bool use_dns = config_getboolean(Configtable,sname,"dns",Global_use_dns);

  if(!use_dns || resolve_mcast(data,&chan_template.output.dest_socket,DEFAULT_RTP_PORT,NULL,0,2) != 0)
    // If we're not using the DNS, or if resolution fails, hash name string to make IP multicast address in 239.x.x.x range
    addr = make_maddr(data);

  {
    size_t slen = sizeof(chan_template.output.dest_socket);
    // there may be several hosts with the same section names
    // prepend the host name to the service name
    char service_name[512] = {0};
    snprintf(service_name, sizeof service_name, "%s %s", Hostname, sname);
    char ttlmsg[128];
    snprintf(ttlmsg,sizeof ttlmsg,"TTL=%d",chan_template.output.ttl);
    char const *cp = config2_getstring(Configtable,Configtable,GLOBAL,sname,"encoding","s16be");
    bool is_opus = strcasecmp(cp,"opus") == 0 ? true : false;
    avahi_start(service_name,
		is_opus ? "_opus._udp" : "_rtp._udp",
		DEFAULT_RTP_PORT,
		data,addr,ttlmsg,
		addr != 0 ? &chan_template.output.dest_socket : NULL,
		addr != 0 ? &slen : NULL);
  }

  // Set up output stream (data + status)
  // data stream is shared by all channels in this section
  // Now also used for per-channel status/control, with different port number
  chan_template.status.dest_socket = chan_template.output.dest_socket;
  setport(&chan_template.status.dest_socket,DEFAULT_STAT_PORT);
  strlcpy(chan_template.output.dest_string,data,sizeof chan_template.output.dest_string);
  chan_template.output.rtp.type = pt_from_info(chan_template.output.samprate,chan_template.output.channels,chan_template.output.encoding);

  char const *iface = NULL;
  if(chan_template.output.ttl != 0){
    // Override global defaults
    iface = config_getstring(Configtable,sname,"iface",Iface);
    join_group(Output_fd,NULL,&chan_template.output.dest_socket,iface);
  }
  // No need to also join group for status socket, since the IP addresses are the same

  // Process frequency/frequencies
  // We need to do this first to ensure the resulting SSRCs are unique
  // To work around iniparser's limited line length, we look for multiple keywords
  // "freq", "freq0", "freq1", etc, up to "freq9"
  int nchans = 0;

  for(int ff = -1; ff < 10; ff++){
    char fname[10];
    if(ff == -1)
      snprintf(fname,sizeof(fname),"freq");
    else
      snprintf(fname,sizeof(fname),"freq%d",ff);

    char const * const frequencies = config_getstring(Configtable,sname,fname,NULL);
    if(frequencies == NULL)
      continue; // none with this prefix; look for more

    // Parse the frequency list(s)
    char *freq_list = strdup(frequencies); // Need writeable copy for strtok
    char *saveptr = NULL;
    for(char const *tok = strtok_r(freq_list," \t",&saveptr);
	tok != NULL;
	tok = strtok_r(NULL," \t",&saveptr)){

      double const f = parse_frequency(tok,true);
      if(f < 0){
	fprintf(stderr,"[%s] can't parse frequency %s\n",sname,tok);
	continue;
      }
      uint32_t ssrc = 0;
      // Generate default ssrc from frequency string
      for(char const *cp = tok ; cp != NULL && *cp != '\0' ; cp++){
	if(isdigit(*cp)){
	  ssrc *= 10;
	  ssrc += *cp - '0';
	}
      }
      ssrc = config_getint(Configtable,sname,"ssrc",ssrc); // Explicitly set?
      if(ssrc == 0)
	continue; // Reserved ssrc

      struct channel *chan = NULL;
      // Try to create it, incrementing in case of collision
      int const max_collisions = 100;
      for(int i=0; i < max_collisions; i++,ssrc++){
	chan = create_chan(ssrc);
	if(chan != NULL)
	  break;
      }
      if(chan == NULL){
	fprintf(stderr,"Can't allocate requested ssrc in range %u-%u\n",ssrc-max_collisions,ssrc);
	continue;
      }
      // Initialize from template, set frequency and start
      // Be careful with shallow copies like this; although the pointers in the channel structure are still NULL
      // the ssrc and inuse fields are active and must be cleaned up. Are there any others...?
      *chan = chan_template;
      chan->output.rtp.ssrc = ssrc; // restore after template copy

      set_freq(chan,f);
      start_demod(chan);
      nchans++;
      Nchans++;

      if(SAP_enable){
	// Highly experimental, off by default
	char sap_dest[] = "224.2.127.254:9875"; // sap.mcast.net
	resolve_mcast(sap_dest,&chan->sap.dest_socket,0,NULL,0,0);
	if(chan_template.output.ttl != 0)
	  join_group(Output_fd,NULL,&chan->sap.dest_socket,iface);
	pthread_create(&chan->sap.thread,NULL,sap_send,chan);
      }
      // RTCP Real Time Control Protocol daemon is optional
      if(RTCP_enable){
	// Set the dest socket to the RTCP port on the output group
	// What messy code just to overwrite a structure field, eh?
	chan->rtcp.dest_socket = chan->output.dest_socket;
	setport(&chan->rtcp.dest_socket,DEFAULT_RTCP_PORT);
	pthread_create(&chan->rtcp.thread,NULL,rtcp_send,chan);
      }
    }
    // Done processing frequency list(s) and creating chans
    FREE(freq_list);
  }
  fprintf(stderr,"[%s] %d channels started\n",sname,nchans);
  return NULL;
}


// Set up a local front end device
static int setup_hardware(char const *sname){
  if(sname == NULL)
   return -1; // Possible?
  char const *device = config_getstring(Configtable,sname,"device",sname);
  // Do we support it?
  // This should go into a table somewhere
#ifdef STATIC
  if(strcasecmp(device,"rx888") == 0){
    Frontend.setup = rx888_setup;
    Frontend.start = rx888_startup;
    Frontend.tune = rx888_tune;
    Frontend.gain = rx888_gain;
    Frontend.atten = rx888_atten;
  } else if(strcasecmp(device,"airspy") == 0){
    Frontend.setup = airspy_setup;
    Frontend.start = airspy_startup;
    Frontend.tune = airspy_tune;
  } else if(strcasecmp(device,"airspyhf") == 0){
    Frontend.setup = airspyhf_setup;
    Frontend.start = airspyhf_startup;
    Frontend.tune = airspyhf_tune;
  } else if(strcasecmp(device,"funcube") == 0){
    Frontend.setup = funcube_setup;
    Frontend.start = funcube_startup;
    Frontend.tune = funcube_tune;
  } else if(strcasecmp(device,"rtlsdr") == 0){
    Frontend.setup = rtlsdr_setup;
    Frontend.start = rtlsdr_startup;
    Frontend.tune = rtlsdr_tune;
  } else if(strcasecmp(device,"sig_gen") == 0) {
    Frontend.setup = sig_gen_setup;
    Frontend.start = sig_gen_startup;
    Frontend.tune = sig_gen_tune;

    /* SDRPlay is now a dynamically loaded module.
       1. Install the API package (I used https://github.com/srcejon/sdrplayapi)
          Note this will also install a strange half-megabyte daemon in /etc/systemd/system/
	  I have no idea what it does, but it burns up much more CPU than all of ka9q-radio
       2. run "make SDRPLAY=1" to build and install the sdrplay.so module so radiod can load it

       It's not built by default because the compile will fail unless the API package is installed,
       and it's just too much hassle for people who don't have an SDRPlay anyway
       The sdrplay library is still proprietary and object-only, so I can't bundle it in ka9q-radio
       Everything else either has a standard Debian package or I have information to program them directly.
       To hell with vendors who deliberately make their products hard to use when they have plenty of competition.
    */
  } else
#endif
  {
    // Try to find it dynamically
    char defname[PATH_MAX];
    snprintf(defname,sizeof(defname),"%s/%s.so",SODIR,device);
    char const *dlname = config_getstring(Configtable,device,"library",defname);
    if(dlname == NULL){
      fprintf(stderr,"No dynamic library specified for device %s\n",device);
      return -1;
    }
    fprintf(stderr,"Dynamically loading %s hardware driver from %s\n",device,dlname);
    char *error;
    Dl_handle = dlopen(dlname,RTLD_GLOBAL|RTLD_NOW);
    if(Dl_handle == NULL){
      error = dlerror();
      fprintf(stderr,"Error loading %s to handle device %s: %s\n",dlname,device,error);
      return -1;
    }
    char symname[128];
    snprintf(symname,sizeof(symname),"%s_setup",device);
    Frontend.setup = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      fprintf(stderr,"error: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
      dlclose(Dl_handle);
      return -1;
    }
    snprintf(symname,sizeof(symname),"%s_startup",device);
    Frontend.start = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      fprintf(stderr,"error: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
      dlclose(Dl_handle);
      return -1;
    }
    snprintf(symname,sizeof(symname),"%s_tune",device);
    Frontend.tune = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      // Not fatal, but no tuning possible
      fprintf(stderr,"warning: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
    }
    // No error checking on these, they're optional
    snprintf(symname,sizeof(symname),"%s_gain",device);
    Frontend.gain = dlsym(Dl_handle,symname);
    snprintf(symname,sizeof(symname),"%s_atten",device);
    Frontend.atten = dlsym(Dl_handle,symname);
  }

  int r = (*Frontend.setup)(&Frontend,Configtable,sname);
  if(r != 0){
    fprintf(stderr,"device setup returned %d\n",r);
    return r;
  }

  // Create input filter now that we know the parameters
  // FFT and filter sizes computed from specified block duration and sample rate
  // L = input data block size
  // M = filter impulse response duration
  // N = FFT size = L + M - 1
  // Note: no checking that N is an efficient FFT blocksize; choose your parameters wisely
  assert(Frontend.samprate != 0);
  double const eL = Frontend.samprate * Blocktime / 1000.0; // Blocktime is in milliseconds
  Frontend.L = lround(eL);
  if(Frontend.L != eL)
    fprintf(stderr,"Warning: non-integral samples in %.3f ms block at sample rate %d Hz: remainder %g\n",
	    Blocktime,Frontend.samprate,eL-Frontend.L);

  Frontend.M = Frontend.L / (Overlap - 1) + 1;
  assert(Frontend.M != 0);
  assert(Frontend.L != 0);
  create_filter_input(&Frontend.in,Frontend.L,Frontend.M, Frontend.isreal ? REAL : COMPLEX);
  // Create list of frequency spurs in filter input (experimental)
  Frontend.in.notches = calloc(100,sizeof (struct notch_state));
  struct notch_state *notch = Frontend.in.notches;
  int const N = Frontend.M + Frontend.L - 1;

  // Initialize spur list. MUST leave last entry zeroed as sentinel; also doubles as 0 Hz (DC) suppression
  for(int i = 0; i < NSPURS; i++){
    int shift;
    double remainder; // Offset from bin center, Hz, e.g, -20 to +20. Or is it -25 to +25?
    int r = compute_tuning(N,Frontend.M,Frontend.samprate,&shift,&remainder,Frontend.spurs[i]);
    if(r != 0)
      break;
    notch->state = 0;
    notch->bin = abs(shift);
    notch->alpha = .01; //  About 10 sec. Arbitrary, make adaptive.
    if(shift == 0) // DC is implicitly last
      break;
    notch++;
  }
  pthread_mutex_init(&Frontend.status_mutex,NULL);
  pthread_cond_init(&Frontend.status_cond,NULL);
  if(Frontend.start){
    int r = (*Frontend.start)(&Frontend);
    if(r != 0)
      fprintf(stderr,"Front end start returned %d\n",r);

    return r;
  } else {
    fprintf(stderr,"No front end start routine?\n");
    return -1;
  }
}


// RTP control protocol sender task
static void *rtcp_send(void *arg){
  struct channel *chan = (struct channel *)arg;
  if(chan == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }

  while(true){

    if(chan->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    uint8_t buffer[PKTSIZE]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));

    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = chan->output.rtp.ssrc;

    // Construct NTP timestamp (NTP uses UTC, ignores leap seconds)
    {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME,&now);
      sr.ntp_timestamp = ((int64_t)now.tv_sec + NTP_EPOCH) << 32;
      sr.ntp_timestamp += ((int64_t)now.tv_nsec << 32) / BILLION; // NTP timestamps are units of 2^-32 sec
    }
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = (0 + gps_time_ns() - Starttime) / BILLION;
    sr.packet_count = chan->output.rtp.seq;
    sr.byte_count = chan->output.rtp.bytes;

    uint8_t *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];

    // CNAME
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",Hostname);
    if(sl > 0 && sl <= 255){
      sdes[0].type = CNAME;
      strlcpy(sdes[0].message,string,sizeof(sdes[0].message));
      sdes[0].mlen = strlen(sdes[0].message);
    }
    FREE(string);

    sdes[1].type = NAME;
    strlcpy(sdes[1].message,"KA9Q Radio Program",sizeof(sdes[1].message));
    sdes[1].mlen = strlen(sdes[1].message);

    sdes[2].type = EMAIL;
    strlcpy(sdes[2].message,"karn@ka9q.net",sizeof(sdes[2].message));
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strlcpy(sdes[3].message,"KA9Q Radio Program",sizeof(sdes[3].message));
    sdes[3].mlen = strlen(sdes[3].message);

    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),chan->output.rtp.ssrc,sdes,4);

    if(sendto(Output_fd,buffer,dp-buffer,0,&chan->rtcp.dest_socket,sizeof(chan->rtcp.dest_socket)) < 0)
      chan->output.errors++;
  done:;
    sleep(1);
  }
}
static void closedown(int a){
  char message[] = "Received signal, shutting down\n";

  int r = write(1,message,strlen(message));
  (void)r; // shut up compiler
  Stop_transfers = true;
  sleep(1); // pause for threads to see it
  _exit(a == SIGTERM ? EX_OK : EX_SOFTWARE); // Success when terminated by systemd
}

// Increase or decrease logging level (thanks AI6VN for idea)
static void verbosity(int a){
  if(a == SIGUSR1)
    Verbose++;
  else if(a == SIGUSR2)
    Verbose = (Verbose <= 0) ? 0 : Verbose - 1;
  else
    return;
}
