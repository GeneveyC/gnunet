/*
     This file is part of GNUnet.
     (C) 2006 Christian Grothoff (and other contributing authors)

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
 * @file src/util/os_installation.c
 * @brief get paths used by the program
 * @author Milan
 */
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "gnunet_common.h"
#include "gnunet_configuration_lib.h"
#include "gnunet_disk_lib.h"
#include "gnunet_os_lib.h"
#if DARWIN
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#endif

#if LINUX
/**
 * Try to determine path by reading /proc/PID/exe
 */
static char *
get_path_from_proc_maps ()
{
  char fn[64];
  char line[1024];
  char dir[1024];
  FILE *f;
  char *lgu;

  GNUNET_snprintf (fn,
		   sizeof(fn), 
		   "/proc/%u/maps", 
		   getpid ());
  f = fopen (fn, "r");
  if (f == NULL)
    return NULL;
  while (NULL != fgets (line, sizeof(line), f))
    {
      if ((1 == sscanf (line,
			"%*x-%*x %*c%*c%*c%*c %*x %*2u:%*2u %*u%*[ ]%s",
			dir)) &&
	  (NULL != (lgu = strstr (dir, "libgnunetutil"))))
	{
	  lgu[0] = '\0';
	  fclose (f);
	  return GNUNET_strdup (dir);
	}
    }
  fclose (f);
  return NULL;
}

/**
 * Try to determine path by reading /proc/PID/exe
 */
static char *
get_path_from_proc_exe ()
{
  char fn[64];
  char lnk[1024];
  ssize_t size;

  GNUNET_snprintf (fn, 
		   sizeof(fn), "/proc/%u/exe", getpid ());
  size = readlink (fn, lnk, sizeof (lnk)-1);
  if (size <= 0)
    {
      GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR, "readlink", fn);
      return NULL;
    }
  GNUNET_assert (size < sizeof (lnk));
  lnk[size] = '\0';
  while ((lnk[size] != '/') && (size > 0))
    size--;
  if ((size < 4) || (lnk[size - 4] != '/'))
    {
      /* not installed in "/bin/" -- binary path probably useless */
      return NULL;
    }
  lnk[size] = '\0';
  return GNUNET_strdup (lnk);
}
#endif

#if WINDOWS
/**
 * Try to determine path with win32-specific function
 */
static char *
get_path_from_module_filename ()
{
  char path[4097];
  char *idx;

  GetModuleFileName (NULL, path, sizeof(path)-1);
  idx = path + strlen (path);
  while ((idx > path) && (*idx != '\\') && (*idx != '/'))
    idx--;
  *idx = '\0';
  return GNUNET_strdup (path);
}
#endif

#if DARWIN
typedef int (*MyNSGetExecutablePathProto) (char *buf, size_t * bufsize);

static char *
get_path_from_NSGetExecutablePath ()
{
  static char zero = '\0';
  char *path;
  size_t len;
  MyNSGetExecutablePathProto func;
  int ret;

  path = NULL;
  func =
    (MyNSGetExecutablePathProto) dlsym (RTLD_DEFAULT, "_NSGetExecutablePath");
  if (!func)
    return NULL;
  path = &zero;
  len = 0;
  /* get the path len, including the trailing \0 */
  func (path, &len);
  if (len == 0)
    return NULL;
  path = GNUNET_malloc (len);
  ret = func (path, &len);
  if (ret != 0)
    {
      GNUNET_free (path);
      return NULL;
    }
  len = strlen (path);
  while ((path[len] != '/') && (len > 0))
    len--;
  path[len] = '\0';
  return path;
}

static char *
get_path_from_dyld_image ()
{
  const char *path;
  char *p, *s;
  int i;
  int c;

  p = NULL;
  c = _dyld_image_count ();
  for (i = 0; i < c; i++)
    {
      if (_dyld_get_image_header (i) == &_mh_dylib_header)
        {
          path = _dyld_get_image_name (i);
          if (path != NULL && strlen (path) > 0)
            {
              p = strdup (path);
              s = p + strlen (p);
              while ((s > p) && (*s != '/'))
                s--;
              s++;
              *s = '\0';
            }
          break;
        }
    }
  return p;
}
#endif

/**
 * Return the actual path to a file found in the current
 * PATH environment variable.
 *
 * @param binary the name of the file to find
 * @return path to binary, NULL if not found
 */
static char *
get_path_from_PATH (const char *binary)
{
  char *path;
  char *pos;
  char *end;
  char *buf;
  const char *p;

  p = getenv ("PATH");
  if (p == NULL)
    return NULL;
  path = GNUNET_strdup (p);     /* because we write on it */
  buf = GNUNET_malloc (strlen (path) + 20);
  pos = path;

  while (NULL != (end = strchr (pos, PATH_SEPARATOR)))
    {
      *end = '\0';
      sprintf (buf, "%s/%s", pos, binary);
      if (GNUNET_DISK_file_test (buf) == GNUNET_YES)
        {
          pos = GNUNET_strdup (pos);
          GNUNET_free (buf);
          GNUNET_free (path);
          return pos;
        }
      pos = end + 1;
    }
  sprintf (buf, "%s/%s", pos, binary);
  if (GNUNET_DISK_file_test (buf) == GNUNET_YES)
    {
      pos = GNUNET_strdup (pos);
      GNUNET_free (buf);
      GNUNET_free (path);
      return pos;
    }
  GNUNET_free (buf);
  GNUNET_free (path);
  return NULL;
}

static char *
get_path_from_GNUNET_PREFIX ()
{
  const char *p;

  p = getenv ("GNUNET_PREFIX");
  if (p != NULL)
    return GNUNET_strdup (p);
  return NULL;
}

/*
 * @brief get the path to GNUnet bin/ or lib/, prefering the lib/ path
 * @author Milan
 *
 * @return a pointer to the executable path, or NULL on error
 */
static char *
os_get_gnunet_path ()
{
  char *ret;

  ret = get_path_from_GNUNET_PREFIX ();
  if (ret != NULL)
    return ret;
#if LINUX
  ret = get_path_from_proc_maps ();
  if (ret != NULL)
    return ret;
  ret = get_path_from_proc_exe ();
  if (ret != NULL)
    return ret;
#endif
#if WINDOWS
  ret = get_path_from_module_filename ();
  if (ret != NULL)
    return ret;
#endif
#if DARWIN
  ret = get_path_from_dyld_image ();
  if (ret != NULL)
    return ret;
  ret = get_path_from_NSGetExecutablePath ();
  if (ret != NULL)
    return ret;
#endif
  ret = get_path_from_PATH ("gnunet-arm");
  if (ret != NULL)
    return ret;
  /* other attempts here */
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              _
              ("Could not determine installation path for %s.  Set `%s' environment variable.\n"),
	      "GNUnet",
              "GNUNET_PREFIX");
  return NULL;
}

/*
 * @brief get the path to current app's bin/
 * @author Milan
 *
 * @return a pointer to the executable path, or NULL on error
 */
static char *
os_get_exec_path ()
{
  char *ret;

  ret = NULL;
#if LINUX
  ret = get_path_from_proc_exe ();
  if (ret != NULL)
    return ret;
#endif
#if WINDOWS
  ret = get_path_from_module_filename ();
  if (ret != NULL)
    return ret;
#endif
#if DARWIN
  ret = get_path_from_NSGetExecutablePath ();
  if (ret != NULL)
    return ret;
#endif
  /* other attempts here */
  return ret;
}



/**
 * @brief get the path to a specific GNUnet installation directory or,
 * with GNUNET_IPK_SELF_PREFIX, the current running apps installation directory
 * @author Milan
 * @return a pointer to the dir path (to be freed by the caller)
 */
char *
GNUNET_OS_installation_get_path (enum GNUNET_OS_InstallationPathKind dirkind)
{
  size_t n;
  const char *dirname;
  char *execpath = NULL;
  char *tmp;
  int isbasedir;

  /* if wanted, try to get the current app's bin/ */
  if (dirkind == GNUNET_OS_IPK_SELF_PREFIX)
    execpath = os_get_exec_path ();

  /* try to get GNUnet's bin/ or lib/, or if previous was unsuccessful some
   * guess for the current app */
  if (execpath == NULL)
    execpath = os_get_gnunet_path ();

  if (execpath == NULL)
    return NULL;

  n = strlen (execpath);
  if (n == 0)
    {
      /* should never happen, but better safe than sorry */
      GNUNET_free (execpath);
      return NULL;
    }
  /* remove filename itself */
  while ((n > 1) && (execpath[n - 1] == DIR_SEPARATOR))
    execpath[--n] = '\0';

  isbasedir = 1;
  if ((n > 5) &&
      ((0 == strcasecmp (&execpath[n - 5], "lib32")) ||
       (0 == strcasecmp (&execpath[n - 5], "lib64"))))
    {
      if (dirkind != GNUNET_OS_IPK_LIBDIR)
        {
          /* strip '/lib32' or '/lib64' */
          execpath[n - 5] = '\0';
          n -= 5;
        }
      else
        isbasedir = 0;
    }
  else if ((n > 3) &&
           ((0 == strcasecmp (&execpath[n - 3], "bin")) ||
            (0 == strcasecmp (&execpath[n - 3], "lib"))))
    {
      /* strip '/bin' or '/lib' */
      execpath[n - 3] = '\0';
      n -= 3;
    }
  /* in case this was a directory named foo-bin, remove "foo-" */
  while ((n > 1) && (execpath[n - 1] == DIR_SEPARATOR))
    execpath[--n] = '\0';
  switch (dirkind)
    {
    case GNUNET_OS_IPK_PREFIX:
    case GNUNET_OS_IPK_SELF_PREFIX:
      dirname = DIR_SEPARATOR_STR;
      break;
    case GNUNET_OS_IPK_BINDIR:
      dirname = DIR_SEPARATOR_STR "bin" DIR_SEPARATOR_STR;
      break;
    case GNUNET_OS_IPK_LIBDIR:
      if (isbasedir)
        dirname =
          DIR_SEPARATOR_STR "lib" DIR_SEPARATOR_STR "gnunet"
          DIR_SEPARATOR_STR;
      else
        dirname = DIR_SEPARATOR_STR "gnunet" DIR_SEPARATOR_STR;
      break;
    case GNUNET_OS_IPK_DATADIR:
      dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "gnunet"
        DIR_SEPARATOR_STR;
      break;
    case GNUNET_OS_IPK_LOCALEDIR:
      dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "locale"
        DIR_SEPARATOR_STR;
      break;
    case GNUNET_OS_IPK_ICONDIR:
      dirname =
        DIR_SEPARATOR_STR "share" DIR_SEPARATOR_STR "icons" DIR_SEPARATOR_STR;
      break;
    default:
      GNUNET_free (execpath);
      return NULL;
    }
  tmp = GNUNET_malloc (strlen (execpath) + strlen (dirname) + 1);
  sprintf (tmp, "%s%s", execpath, dirname);
  GNUNET_free (execpath);
  return tmp;
}


/**
 * Check whether the suid bit is set on a file.
 * Attempts to find the file using the current
 * PATH environment variable as a search path.
 *
 * @param binary the name of the file to check
 * @return GNUNET_YES if the file is SUID, 
 *         GNUNET_NO if not, 
 *         GNUNET_SYSERR on error
 */
int
GNUNET_OS_check_helper_binary (const char *binary)
{
  struct stat statbuf;
  char *p;
#ifdef MINGW
  SOCKET rawsock;
  char *binaryexe;

  GNUNET_asprintf (&binaryexe, "%s.exe", binary);
  p = get_path_from_PATH (binaryexe);
  free (binaryexe);
#else
  p = get_path_from_PATH (binary);
#endif
  if (p == NULL)
    {
      GNUNET_log_from (GNUNET_ERROR_TYPE_ERROR,
		       "tcp",
		       _("Could not find binary `%s' in PATH!\n"),
		       binary);
      return GNUNET_NO;
    }
  if (0 != STAT (p, &statbuf))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING, 
		  _("stat (%s) failed: %s\n"), 
		  p,
		  STRERROR (errno));
      GNUNET_free (p);
      return GNUNET_SYSERR;
    }
  GNUNET_free (p);
#ifndef MINGW
  if ( (0 != (statbuf.st_mode & S_ISUID)) &&
       (statbuf.st_uid == 0) )
    return GNUNET_YES;
  return GNUNET_NO;
#else
  rawsock = socket (AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (INVALID_SOCKET == rawsock)
    {
      DWORD err = GetLastError ();
      GNUNET_log_from (GNUNET_ERROR_TYPE_WARNING, 
		       "tcp",
		       "socket (AF_INET, SOCK_RAW, IPPROTO_ICMP) failed! GLE = %d\n", err);
      return GNUNET_NO; /* not running as administrator */
    }
  closesocket (rawsock);
  return GNUNET_YES;
#endif
}


/* end of os_installation.c */
