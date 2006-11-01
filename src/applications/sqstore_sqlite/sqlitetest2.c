/*
     This file is part of GNUnet.
     (C) 2004, 2005, 2006 Christian Grothoff (and other contributing authors)

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
/*
 * @file applications/sqstore_sqlite/sqlitetest2.c
 * @brief Test for the sqstore implementations.
 * @author Christian Grothoff
 *
 * This testcase inserts a bunch of (variable size) data and then deletes
 * data until the (reported) database size drops below a given threshold.
 * This is iterated 10 times, with the actual size of the content stored,
 * the database size reported and the file size on disk being printed for
 * each iteration.  The code also prints a "I" for every 40 blocks
 * inserted and a "D" for every 40 blocks deleted.  The deletion 
 * strategy alternates between "lowest priority" and "earliest expiration".
 * Priorities and expiration dates are set using a pseudo-random value 
 * within a realistic range.
 */

#include "platform.h"
#include "gnunet_util.h"
#include "gnunet_util_cron.h"
#include "gnunet_util_crypto.h"
#include "gnunet_util_config_impl.h"
#include "gnunet_protocols.h"
#include "gnunet_sqstore_service.h"
#include "core.h"

#define ASSERT(x) do { if (! (x)) { printf("Error at %s:%d\n", __FILE__, __LINE__); goto FAILURE;} } while (0)

/**
 * Target datastore size (in bytes).
 * <p>
 * 
 * Example impact of total size on the reported number
 * of operations (insert and delete) per second (once
 * stabilized) for a particular machine:
 * <pre>
 *   1: 824
 *   2: 650
 *   4: 350
 *   8: 343
 *  16: 230
 *  32: 131
 *  64:  70
 * 128:  47
 * </pre>
 * <p>
 * This was measured on a machine with 4 GB main 
 * memory and under 100% CPU load, so disk overhead
 * is NOT relevant for the performance loss for
 * larger sizes.  This seems to indicate that
 * at least one of the performed operation does not
 * yet scale to larger sizes!  This is most likely
 * the delete operation -- initial pure insertion 
 * peaks at about 2500 operations per second!<br>
 *
 * <p>
 * The disk size overhead (additional disk space used
 * compared to actual data stored) for all sizes
 * was around 12-17%.  The API-reported size was 
 * 12.74 bytes per entry more than the actual data 
 * stored (which is a good estimate of the module's
 * internal overhead).  
 */
#define MAX_SIZE 1024 * 1024 * 4

/**
 * Report progress outside of major reports? Should probably be YES if
 * size is > 16 MB.
 */
#define REPORT_ID NO

/**
 * Number of put operations equivalent to 1/10th of MAX_SIZE 
 */
#define PUT_10 MAX_SIZE / 32 / 1024 / 10

/**
 * Progress report frequency.  1/10th of a put operation block.
 */
#define REP_FREQ PUT_10 / 10

/**
 * Total number of iterations (each iteration doing
 * PUT_10 put operations); we report full status every
 * 10 iterations.  Abort with CTRL-C.
 */
#define ITERATIONS 100

/**
 * Name of the database on disk.
 */
#define DB_NAME "/tmp/gnunet-sqlite-sqstore-test/data/fs/content/gnunet.dat"

static unsigned long long stored_bytes;

static unsigned long long stored_entries;

static unsigned long long stored_ops;

static cron_t start_time;

static int putValue(SQstore_ServiceAPI * api,
		    int i) {
  Datastore_Value * value;
  size_t size;
  static HashCode512 key;
  static int ic;
  
  /* most content is 32k */
  size = sizeof(Datastore_Value) + 32 * 1024;
  if (weak_randomi(16) == 0) /* but some of it is less! */
    size = sizeof(Datastore_Value) + weak_randomi(32 * 1024);
  size = size - (size & 7); /* always multiple of 8 */

  /* generate random key */
  hash(&key,
       sizeof(HashCode512),
       &key);
  value = MALLOC(size);
  value->size = htonl(size);
  value->type = htonl(i);
  value->prio = htonl(weak_randomi(100));
  value->anonymityLevel = htonl(i);
  value->expirationTime = htonll(get_time() + weak_randomi(1000));
  memset(&value[1], 
	 i, 
	 size - sizeof(Datastore_Value));
  if (OK != api->put(&key, value)) {
    FREE(value);
    fprintf(stderr, "E");
    return SYSERR;
  }
  ic++;
#if REPORT_ID
  if (ic % REP_FREQ == 0)
    fprintf(stderr, "I");
#endif
  stored_bytes += ntohl(value->size);
  stored_ops++;
  stored_entries++;
  FREE(value);
  return OK;
}

static int 
iterateDelete(const HashCode512 * key,
	      const Datastore_Value * val,
	      void * cls) {
  SQstore_ServiceAPI * api = cls;
  static int dc;

  if (api->getSize() < MAX_SIZE)
    return SYSERR;
  if (GNUNET_SHUTDOWN_TEST() == YES)
    return SYSERR;
  dc++;
#if REPORT_ID
  if (dc % REP_FREQ == 0)
    fprintf(stderr, "D");
#endif
  GE_ASSERT(NULL, 1 == api->del(key, val));
  stored_bytes -= ntohl(val->size);
  stored_entries--;
  return OK;
}

/**
 * Add testcode here!
 */
static int test(SQstore_ServiceAPI * api) {
  int i;
  int j;
  unsigned long long size;
  int have_file;

  have_file = OK == disk_file_test(NULL,
				   DB_NAME);

  for (i=0;i<ITERATIONS;i++) {
#if REPORT_ID
    fprintf(stderr, ".");
#endif
    /* insert data equivalent to 1/10th of MAX_SIZE */
    for (j=0;j<PUT_10;j++) {
      ASSERT(OK == putValue(api, j));
      if (GNUNET_SHUTDOWN_TEST() == YES)
	break;
    }

    /* trim down below MAX_SIZE again */
    if ((i % 2) == 0)
      api->iterateLowPriority(0, &iterateDelete, api);
    else
      api->iterateExpirationTime(0, &iterateDelete, api);    

    /* every 10 iterations print status */
    size = 0;
    if (have_file)
      disk_file_size(NULL,
		     DB_NAME,
		     &size,
		     NO);
    printf(
#if REPORT_ID
	   "\n"
#endif
	   "Useful %llu, API %llu (Useful-API: %lld/%.2f), disk %llu (%.2f%%) / %lluk ops / %llu ops/s\n",
	   stored_bytes / 1024,  /* used size in k */
	   api->getSize() / 1024, /* API-reported size in k */
	   (api->getSize() - stored_bytes) / 1024, /* difference between reported and used */
	   1.0 * (api->getSize() - stored_bytes) / (stored_entries * sizeof(Datastore_Value)), /* relative to number of entries (should be equal to internal overhead per entry) */
	   size / 1024, /* disk size in kb */
	   (100.0 * size / stored_bytes) - 100, /* overhead */
	   (stored_ops * 2 - stored_entries) / 1024, /* total operations (in k) */
	   1000 * (stored_ops * 2 - stored_entries) / (1 + get_time() - start_time)); /* operations per second */  
    if (GNUNET_SHUTDOWN_TEST() == YES)
      break;
  }
  api->drop();
  return OK;

 FAILURE:
  api->drop();
  return SYSERR;
}

int main(int argc, char *argv[]) {
  SQstore_ServiceAPI * api;
  int ok;
  struct GC_Configuration * cfg;
  struct CronManager * cron;

  cfg = GC_create_C_impl();
  if (-1 == GC_parse_configuration(cfg,
				   "check.conf")) {
    GC_free(cfg);
    return -1;
  }
  cron = cron_create(NULL);
  initCore(NULL,
	   cfg,
	   cron,
	   NULL);
  api = requestService("sqstore");
  if (api != NULL) {
    start_time = get_time();
    ok = test(api);
    releaseService(api);
  } else
    ok = SYSERR;
  doneCore();
  cron_destroy(cron);
  GC_free(cfg);
  if (ok == SYSERR)
    return 1;
  return 0;
}

/* end of sqlitetest2.c */
