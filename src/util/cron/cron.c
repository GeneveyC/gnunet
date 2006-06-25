/*
     This file is part of GNUnet.
     (C) 2001, 2002, 2003, 2004, 2005, 2006 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @file util/pthreads/cron.c
 * @author Christian Grothoff
 * @brief Module for periodic background (cron) jobs.
 *
 * The module only uses one thread, thus every cron-job must be
 * short-lived, should never block for an indefinite amount of
 * time. Specified deadlines are only a guide-line, the 10ms
 * timer-resolution is only an upper-bound on the possible precision,
 * in practice it will be worse (depending on the other cron-jobs).
 *
 * If you need to schedule a long-running or blocking cron-job,
 * run a function that will start another thread that will
 * then run the actual job.
 */

#include "gnunet_util_cron.h"
#include "platform.h"

#define DEBUG_CRON NO

#if DEBUG_CRON
#define HAVE_PRINT_CRON_TAB 1
#else
#define HAVE_PRINT_CRON_TAB 0
#endif

/**
 * The initial size of the cron-job table
 */
#define INIT_CRON_JOBS 16

/**
 * how long do we sleep at most? In some systems, the
 * signal-interrupted sleep does not work nicely, so to ensure
 * progress, we should rather wake up periodically. (But we don't want
 * to burn too much CPU time doing busy waiting; every 2s strikes a
 * good balance)
 */
#define MAXSLEEP 2000

#define CHECK_ASSERTS 1

/* change this value to artificially speed up all
   GNUnet cron timers by this factor. E.g. with 10,
   a cron-job scheduled after 1 minute in the code
   will occur after 6 seconds. This is useful for
   testing bugs that would otherwise occur only after
   a long time.

   For releases, this value should always be 1 */
#define SPEED_UP 1

/** number of cron units (ms) in a second */
#define CRON_UNIT_TO_SECONDS (1000 / SPEED_UP)

/** number of us [usec] in a cron-unit (1000) */
#define MICROSEC_TO_CRON_UNIT (1000 * SPEED_UP)


/**
 * @brief The Delta-list for the cron jobs.
 */
typedef struct {

  /**
   * The method to call at that point. 
   */
  CronJob method;

  /**
   * data ptr (argument to the method) 
   */
  void * data;

  /**
   * The start-time for this event (in milliseconds). 
   */
  cron_t delta;

  /**
   * for cron-jobs: when this should be repeated
   * automatically, 0 if this was a once-only job 
   */
  unsigned int deltaRepeat;

  /**
   * The index of the next entry in the delta list
   * after this one (-1 for none)
   */
  int next;

} UTIL_cron_DeltaListEntry;

typedef struct CronManager {

  /**
   * The lock for the delta-list.
   */
  struct MUTEX * deltaListLock_;

  /**
   * The delta-list of waiting tasks.
   */
  UTIL_cron_DeltaListEntry * deltaList_;

  /**
   * The currently running job.
   */
  CronJob runningJob_;

  void * runningData_;

  /**
   * The cron thread.
   */
  struct PTHREAD * cron_handle;

  struct SEMAPHORE * cron_signal;

  struct SEMAPHORE * cron_signal_up;

  struct MUTEX * inBlockLock_;

  unsigned int runningRepeat_;

  /**
   * The current size of the DeltaList.
   */
  unsigned int deltaListSize_;

  /**
   * The first empty slot in the delta-list.
   */
  int firstFree_;

  /**
   * The first empty slot in the delta-list.
   */
  int firstUsed_;

  /**
   * Set to yes if we are shutting down or shut down.
   */ 
  int cron_shutdown;

} CronManager;


/**
 * Initialize the cron service.
 */
void initCron() {
  unsigned int i;
#ifndef MINGW
  static struct sigaction sig;
  static struct sigaction old;
#endif

  deltaListSize_ = INIT_CRON_JOBS;
  deltaList_
    = MALLOC(sizeof(UTIL_cron_DeltaListEntry) * deltaListSize_);
  for (i=0;i<deltaListSize_;i++)
    deltaList_[i].next = i-1;
  firstFree_ = deltaListSize_-1;
  MUTEX_CREATE_RECURSIVE(&deltaListLock_);
  MUTEX_CREATE(&inBlockLock_);
  runningJob_ = NULL;
  firstUsed_  = -1;
  /* SA_NODEFER == SA_NOMASK but is available on linux */

  cron_signal_up = SEMAPHORE_NEW(0);
}

static void noJob(void * unused) {
#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "In noJob.\n");
#endif
}

/**
 * Stop the cron service.
 */
void stopCron() {
  void * unused;

#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "Stopping cron\n");
#endif
  cron_shutdown = YES;
  addCronJob(&noJob, 0, 0, NULL);
  SEMAPHORE_DOWN(cron_signal);
  SEMAPHORE_FREE(cron_signal);
  cron_signal = NULL;
  PTHREAD_JOIN(&cron_handle, &unused);
#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "Cron stopped\n");
#endif
}

static int inBlock = 0;

/**
 * CronJob to suspend the cron thread
 * until it is resumed.
 */
static void block(void * sem) {
  Semaphore * sig = sem;
  int ok = SYSERR;

  if (sig != NULL)
    SEMAPHORE_UP(sig);
  while (ok == SYSERR) {
    SEMAPHORE_DOWN(cron_signal_up);
    MUTEX_LOCK(&inBlockLock_);
    inBlock--;
    if (inBlock == 0)
      ok = OK;
    MUTEX_UNLOCK(&inBlockLock_);
  }
}
				
/**
 * Stop running cron-jobs for a short time.  This method may only be
 * called by a thread that is not holding any locks (otherwise
 * there is the danger of a deadlock).
 */
void suspendCron() {
  Semaphore * blockSignal;

  GE_ASSERT(NULL, cron_shutdown == NO);
  GE_ASSERT(NULL, NO == PTHREAD_SELF_TEST(&cron_handle));
  MUTEX_LOCK(&inBlockLock_);
  inBlock++;
  if (inBlock == 1) {
    blockSignal = SEMAPHORE_NEW(0);
    addCronJob(&block,
	       0,
	       0,
	       blockSignal);
    SEMAPHORE_DOWN(blockSignal);
    SEMAPHORE_FREE(blockSignal);
  }
  MUTEX_UNLOCK(&inBlockLock_);
}

int isCronRunning() {
  if ( (NO == cron_shutdown) || (inBlock > 0) )
    return YES;
  else
    return NO;
}

/**
 * Resume running cron-jobs.
 */
void resumeCron() {
  GE_ASSERT(NULL, inBlock > 0);
  SEMAPHORE_UP(cron_signal_up);
}

void suspendIfNotCron() {
  if (NO == PTHREAD_SELF_TEST(&cron_handle))
    suspendCron();
}

void resumeIfNotCron() {
  if (NO == PTHREAD_SELF_TEST(&cron_handle))
    resumeCron();
}

static void abortSleep() {
  if (cron_signal == NULL)
    return; /* cron_handle not valid */
  PTHREAD_KILL(&cron_handle, SIGALRM);
}


#if HAVE_PRINT_CRON_TAB
/**
 * Print the cron-tab.
 */
void printCronTab() {
  int jobId;
  UTIL_cron_DeltaListEntry * tab;
  cron_t now;

  cronTime(&now);
  MUTEX_LOCK(&deltaListLock_);

  jobId = firstUsed_;
  while (jobId != -1) {
    tab = &deltaList_[jobId];
    GE_LOG(NULL,
	   GE_STATUS | GE_DEVELOPER | GE_BULK,
	   "%3u: delta %8lld CU --- method %p --- repeat %8u CU\n",
	   jobId,
	   tab->delta - now,
	   (int)tab->method,
	   tab->deltaRepeat);
    jobId = tab->next;
  }
  MUTEX_UNLOCK(&deltaListLock_);
}
#endif


/**
 * If the specified cron-job exists in th delta-list, move it to the
 * head of the list.  If it is running, do nothing.  If it is does not
 * exist and is not running, add it to the list to run it next.
 *
 * @param method which method should we run
 * @param deltaRepeat if this is a periodic, the time between
 *        the runs, otherwise 0.
 * @param data extra argument to calls to method, freed if
 *        non-null and cron is shutdown before the job is
 *        run and/or delCronJob is called
 */
void advanceCronJob(CronJob method,
		    unsigned int deltaRepeat,
		    void * data) {
  UTIL_cron_DeltaListEntry * job;
  UTIL_cron_DeltaListEntry * last;
  int jobId;

#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "Advancing job %p-%p\n",
	 method,
	 data);
#endif
  MUTEX_LOCK(&deltaListLock_);
  jobId = firstUsed_;
  if (jobId == -1) {
    /* not in queue; add if not running */
    if ( (method != runningJob_) ||
         (data != runningData_) ||
	 (deltaRepeat != runningRepeat_) ) {
      GE_LOG(NULL,
	     GE_ERROR | GE_USER | GE_DEVELOPER | GE_BULK,
	     _("`%s' called with cron job not in queue, adding.  This may not be what you want.\n"),
	     __FUNCTION__);
      addCronJob(method,
      		 0,
		 deltaRepeat,
		 data);
    }
    MUTEX_UNLOCK(&deltaListLock_);
    return;
  }
  last = NULL;
  job = &deltaList_[jobId];
  while ( (job->method != method) ||
	  (job->data != data) ||
	  (job->deltaRepeat != deltaRepeat) ) {
    last = job;
    if (job->next == -1) {
      /* not in queue; add if not running */
      if ( (method != runningJob_) ||
	   (data != runningData_) ||
	   (deltaRepeat != runningRepeat_) ) {
	addCronJob(method,
		   0,
		   deltaRepeat,
		   data);
      }
      MUTEX_UNLOCK(&deltaListLock_);
      return;
    }
    jobId = job->next;
    job = &deltaList_[jobId];
  }
  /* ok, found it; remove, re-add with time 0 */
  delCronJob(method,
	     deltaRepeat,
	     data);
  addCronJob(method,
	     0,
	     deltaRepeat,
	     data);
  MUTEX_UNLOCK(&deltaListLock_);
}

/**
 * Add a cron-job to the delta list.
 *
 * @param method which method should we run
 * @param delta how many milliseconds until we run the method
 * @param deltaRepeat if this is a periodic, the time between
 *        the runs, otherwise 0.
 * @param data extra argument to calls to method, freed if
 *        non-null and cron is shutdown before the job is
 *        run and/or delCronJob is called
 */
void addCronJob(CronJob method,
		unsigned int delta,
		unsigned int deltaRepeat,
		void * data) {
  UTIL_cron_DeltaListEntry * entry;
  UTIL_cron_DeltaListEntry * pos;
  int last;
  int current;

#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "Adding job %p-%p to fire in %d CU\n",
	 method,
	 data,
	 delta);
#endif

  MUTEX_LOCK(&deltaListLock_);
  if (firstFree_ == -1) { /* need to grow */
    unsigned int i;

    GROW(deltaList_,
	 deltaListSize_,
	 deltaListSize_ * 2);
    for (i=deltaListSize_/2;i<deltaListSize_;i++)
      deltaList_[i].next = i-1;
    deltaList_[deltaListSize_/2].next = -1;
    firstFree_ = deltaListSize_-1;
  }
  entry = &deltaList_[firstFree_];
  entry->method = method;
  entry->data = data;
  entry->deltaRepeat = deltaRepeat;
  entry->delta = cronTime(NULL) + delta;
  if (firstUsed_ == -1) {
    firstUsed_
      = firstFree_;
    firstFree_
      = entry->next;
    entry->next = -1; /* end of list */
    MUTEX_UNLOCK(&deltaListLock_);
    /* interrupt sleeping cron-thread! */
    abortSleep();
    return;
  }
  /* no, there are jobs waiting */
  last = -1;
  current = firstUsed_;
  pos = &deltaList_[current];

  while (entry->delta > pos->delta) {
    if (pos->next != -1) {
      last = current;
      current = pos->next;
      pos = &deltaList_[current];
    } else { /* append */
      pos->next = firstFree_;
      firstFree_
	= entry->next;
      entry->next = -1;
      MUTEX_UNLOCK(&deltaListLock_);
#if HAVE_PRINT_CRON_TAB
      printCronTab();
#endif
      return;
    }
  }
  /* insert before pos */
  if (last == -1) {
    firstUsed_ = firstFree_;
    abortSleep();
  } else {
    deltaList_[last].next = firstFree_;
#if HAVE_PRINT_CRON_TAB
    printCronTab();
#endif
  }
  firstFree_
    = entry->next;
  entry->next = current;
  MUTEX_UNLOCK(&deltaListLock_);
}

/**
 * Process the cron-job at the beginning of the waiting queue, that
 * is, remove, invoke, and re-insert if it is a periodical job. Make
 * sure the cron job is held when calling this method, but
 * note that it will be released briefly for the time
 * where the job is running (the job to run may add other
 * jobs!)
 */
static void runJob() {
  UTIL_cron_DeltaListEntry * job;
  int jobId;
  CronJob method;
  void * data;
  unsigned int repeat;

  jobId = firstUsed_;
  if (jobId == -1)
    return; /* no job to be done */
  job    = &deltaList_[jobId];
  method = job->method;
  runningJob_ = method;
  data   = job->data;
  runningData_ = data;
  repeat = job->deltaRepeat;
  runningRepeat_ = repeat;
  /* remove from queue */
  firstUsed_
    = job->next;
  job->next
    = firstFree_;
  firstFree_ = jobId;
  MUTEX_UNLOCK(&deltaListLock_);
  /* re-insert */
  if (repeat > 0) {
#if DEBUG_CRON
    GE_LOG(NULL,
	   GE_STATUS | GE_DEVELOPER | GE_BULK,
	   "adding periodic job %p-%p to run again in %u\n",
	   method,
	   data,
	   repeat);
#endif
    addCronJob(method, repeat, repeat, data);
  }
  /* run */
#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "running job %p-%p\n",
	 method,
	 data);
#endif
  method(data);
  MUTEX_LOCK(&deltaListLock_);
  runningJob_ = NULL;
#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "job %p-%p done\n",
	 method,
	 data);
#endif
}

/**
 * The main-method of cron.
 */
static void * cron(void * unused) {
  cron_t now;
  cron_t next;

  while (cron_shutdown == NO) {
#if HAVE_PRINT_CRON_TAB
    printCronTab();
#endif
    cronTime(&now);
    next = now + 0xFFFFFFFF;
    MUTEX_LOCK(&deltaListLock_);
    while (firstUsed_ != -1) {
      cronTime(&now);
      next = deltaList_[firstUsed_].delta;
      if (next <= now) {
#if DEBUG_CRON
	LOG(LOG_CRON,
	    "running cron job, table is\n");
	printCronTab();
#endif
	runJob();
#if DEBUG_CRON
	GE_LOG(NULL,
	       GE_STATUS | GE_DEVELOPER | GE_BULK,
	       "job run, new table is\n");
	printCronTab();
#endif
      } else
	break;
    }
    MUTEX_UNLOCK(&deltaListLock_);
    next = next - now; /* how long to sleep */
#if DEBUG_CRON
    GE_LOG(NULL,
	   GE_STATUS | GE_DEVELOPER | GE_BULK,
	   "Sleeping at %llu for %llu CU (%llu s, %llu CU)\n",
	   now,
	   next,
	   next / cronSECONDS,
	   next);
#endif
    if (next > MAXSLEEP)
      next = MAXSLEEP;
    if (cron_shutdown == NO) 
      gnunet_util_sleep(next);
#if DEBUG_CRON
    GE_LOG(NULL,
	   GE_STATUS | GE_DEVELOPER | GE_BULK,
	   "woke up at  %llu - %lld CS late\n",
	   cronTime(NULL),
	   cronTime(NULL)-(now+next));
#endif
  }
  SEMAPHORE_UP(cron_signal);
#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "Cron thread exits.\n");
  printCronTab();
#endif
  return NULL;
}


/**
 * Make sure to call stopCron before calling this method!
 */
void doneCron() {
  int i;

  i = firstUsed_;
  while (i != -1) {
    FREENONNULL(deltaList_[i].data);
    i = deltaList_[i].next;
  }
  MUTEX_DESTROY(&deltaListLock_);
  MUTEX_DESTROY(&inBlockLock_);
  FREE(deltaList_);
  SEMAPHORE_FREE(cron_signal_up);
  deltaList_ = NULL;
}

/**
 * Start the cron jobs.
 */
void startCron() {
  GE_ASSERT(NULL, cron_signal == NULL);
  cron_shutdown = NO;
  cron_signal = SEMAPHORE_NEW(0);
  /* large stack, we don't know for sure
     what the cron jobs may be doing */
  if (0 != PTHREAD_CREATE(&cron_handle,
			  &cron,
			  NULL,
			  256 * 1024))
    GE_DIE_STRERROR(NULL, 
		    GE_FATAL | GE_ADMIN | GE_USER | GE_BULK, 
		    "pthread_create");
}


/**
 * Remove all matching cron-jobs from the list. This method should
 * only be called while cron is suspended or stopped, or from a cron
 * job that deletes another cron job.  If cron is not suspended or
 * stopped, it may be running the method that is to be deleted, which
 * could be bad (in this case, the deletion will not affect the
 * running job and may return before the running job has terminated).
 *
 * @param method which method is listed?
 * @param repeat which repeat factor was chosen?
 * @param data what was the data given to the method
 * @return the number of jobs removed
 */
int delCronJob(CronJob method,
	       unsigned int repeat,
	       void * data) {
  UTIL_cron_DeltaListEntry * job;
  UTIL_cron_DeltaListEntry * last;
  int jobId;

#if DEBUG_CRON
  GE_LOG(NULL,
	 GE_STATUS | GE_DEVELOPER | GE_BULK,
	 "deleting job %p-%p\n",
	 method,
	 data);
#endif
  MUTEX_LOCK(&deltaListLock_);
  jobId = firstUsed_;
  if (jobId == -1) {
    MUTEX_UNLOCK(&deltaListLock_);
    return 0;
  }
  last = NULL;
  job = &deltaList_[jobId];
  while ( (job->method != method) ||
	  (job->data != data) ||
	  (job->deltaRepeat != repeat) ) {
    last = job;
    if (job->next == -1) {
      MUTEX_UNLOCK(&deltaListLock_);
      return 0;
    }
    jobId = job->next;
    job = &deltaList_[jobId];
  }
  if (last != NULL)
    last->next = job->next;
  else
    firstUsed_ = job->next;
  job->next
    = firstFree_;
  firstFree_ = jobId;
  job->method = NULL;
  job->data = NULL;
  job->deltaRepeat = 0;
  MUTEX_UNLOCK(&deltaListLock_);
  /* ok, there may be more matches, go again! */
  return 1 + delCronJob(method, repeat, data);
}





/* end of cron.c */
