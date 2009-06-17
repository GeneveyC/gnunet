/*
     This file is part of GNUnet.
     (C) 2004, 2005, 2006, 2007 Christian Grothoff (and other contributing authors)

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
 * @file datastore/perf_datastore_api.c
 * @brief performance measurement for the datastore implementation
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
 * <p>
 *
 * Note that the disk overhead calculations are not very sane for
 * MySQL: we take the entire /var/lib/mysql directory (best we can
 * do for ISAM), which may contain other data and which never
 * shrinks.  The scanning of the entire mysql directory during
 * each report is also likely to be the cause of a minor
 * slowdown compared to sqlite.<p>
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_protocols.h"
#include "gnunet_datastore_service.h"

static struct GNUNET_DATASTORE_Handle *datastore;

/**
 * Target datastore size (in bytes).
 * <p>
 * Example impact of total size on the reported number
 * of operations (insert and delete) per second (once
 * roughly stabilized -- this is not "sound" experimental
 * data but just a rough idea) for a particular machine:
 * <pre>
 *    4: 60   at   7k ops total
 *    8: 50   at   3k ops total
 *   16: 48   at   8k ops total
 *   32: 46   at   8k ops total
 *   64: 61   at   9k ops total
 *  128: 89   at   9k ops total
 * 4092: 11   at 383k ops total (12 GB stored, 14.8 GB DB size on disk, 2.5 GB reported)
 * </pre>
 * Pure insertion performance into an empty DB initially peaks
 * at about 400 ops.  The performance seems to drop especially
 * once the existing (fragmented) ISAM space is filled up and
 * the DB needs to grow on disk.  This could be explained with
 * ISAM looking more carefully for defragmentation opportunities.
 * <p>
 * MySQL disk space overheads (for otherwise unused database when
 * run with 128 MB target data size; actual size 651 MB, useful
 * data stored 520 MB) are quite large in the range of 25-30%.
 * <p>
 * This kind of processing seems to be IO bound (system is roughly
 * at 90% wait, 10% CPU).  This is with MySQL 5.0.
 *
 */
#define MAX_SIZE 1024LL * 1024 * 16

/**
 * Report progress outside of major reports? Should probably be GNUNET_YES if
 * size is > 16 MB.
 */
#define REPORT_ID GNUNET_NO

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
 * You may have to adjust this path and the access
 * permission to the respective directory in order
 * to obtain all of the performance information.
 */
#define DB_NAME "/tmp/gnunet-datastore-test/data/fs/"

static unsigned long long stored_bytes;

static unsigned long long stored_entries;

static unsigned long long stored_ops;

static struct GNUNET_TIME_Absolute start_time;

static int
putValue (int i, int k)
{
  size_t size;
  static GNUNET_HashCode key;
  static int ic;
  static char data[65536];

  /* most content is 32k */
  size = 32 * 1024;
  if (GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, 16) == 0)  /* but some of it is less! */
    size = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, 32 * 1024);
  size = size - (size & 7);     /* always multiple of 8 */

  GNUNET_CRYPTO_hash (&key, sizeof (GNUNET_HashCode), &key);
  memset (data, i, size);
  if (i > 255)
    memset (data, i - 255, size / 2);
  data[0] = k;
  GNUNET_DATASTORE_put (datastore,
			0,
			&key,
			size,
			data,
			i,
			GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, 100),
			i,
			GNUNET_TIME_relative_to_absolute 
			(GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS,
							GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_WEAK, 1000))),
			NULL, NULL);
  ic++;
#if REPORT_ID
  if (ic % REP_FREQ == 0)
    fprintf (stderr, "I");
#endif
  stored_bytes += size;
  stored_ops++;
  stored_entries++;
  return GNUNET_OK;
}


static void
iterate_delete (void *cls,
		const GNUNET_HashCode * key,
		uint32_t size,
		const void *data,
		uint32_t type,
		uint32_t priority,
		uint32_t anonymity,
		struct GNUNET_TIME_Absolute
		expiration, uint64_t uid)
{
  GNUNET_DATASTORE_remove (datastore, key, size, data, NULL, NULL);
}



static void
run (void *cls,
     struct GNUNET_SCHEDULER_Handle *sched,
     char *const *args,
     const char *cfgfile, struct GNUNET_CONFIGURATION_Handle *cfg)
{
  int j;
  unsigned long long size;
  int have_file;
  struct stat sbuf;
  int i;

  datastore = GNUNET_DATASTORE_connect (cfg, sched);
  have_file = 0 == stat (DB_NAME, &sbuf);
  for (i = 0; i < ITERATIONS; i++)
    {
#if REPORT_ID
      fprintf (stderr, ".");
#endif
      /* insert data equivalent to 1/10th of MAX_SIZE */
      for (j = 0; j < PUT_10; j++)
	GNUNET_assert (GNUNET_OK == putValue (j, i));

      /* trim down below MAX_SIZE again */
      if ((i % 2) == 0)
        GNUNET_DATASTORE_get_random (datastore, &iterate_delete, NULL);
      size = 0;
      if (have_file)
        GNUNET_disk_file_size (NULL, DB_NAME, &size, GNUNET_NO);
      printf (
#if REPORT_ID
               "\n"
#endif
               "Useful %llu, disk %llu (%.2f%%) / %lluk ops / %llu ops/s\n", 
	       stored_bytes / 1024,     /* used size in k */
               size / 1024,     /* disk size in kb */
               (100.0 * size / stored_bytes) - 100,     /* overhead */
               (stored_ops * 2 - stored_entries) / 1024,        /* total operations (in k) */
               1000 * (stored_ops * 2 - stored_entries) / (1 + GNUNET_get_time () - start_time));       /* operations per second */
      if (GNUNET_shutdown_test () == GNUNET_YES)
        break;
    }

  GNUNET_DATASTORE_disconnect (datastore, GNUNET_YES);
}


static int
check ()
{
  int ok = 1 + 2 + 4 + 8;
  pid_t pid;
  char *const argv[] = { "perf-datastore-api",
    "-c",
    "test_datastore_api_data.conf",
#if VERBOSE
    "-L", "DEBUG",
#endif
    NULL
  };
  struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };
  pid = GNUNET_OS_start_process ("gnunet-service-datastore",
                                 "gnunet-service-datastore",
#if VERBOSE
                                 "-L", "DEBUG",
#endif
                                 "-c", "perf_datastore_api_data.conf", NULL);
  sleep (1);
  GNUNET_PROGRAM_run ((sizeof (argv) / sizeof (char *)) - 1,
                      argv, "perf-datastore-api", "nohelp",
                      options, &run, &ok);
  if (0 != PLIBC_KILL (pid, SIGTERM))
    {
      GNUNET_log_strerror (GNUNET_ERROR_TYPE_WARNING, "kill");
      ok = 1;
    }
  GNUNET_OS_process_wait(pid);
  if (ok != 0)
    fprintf (stderr, "Missed some testcases: %u\n", ok);
  return ok;
}


int
main (int argc, char *argv[])
{
  int ret;

  GNUNET_log_setup ("perf-datastore-api",
#if VERBOSE
                    "DEBUG",
#else
                    "WARNING",
#endif
                    NULL);
  ret = check ();

  return ret;
}





/* end of perf_datastore_api.c */
