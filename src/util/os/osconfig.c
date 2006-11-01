/*
     This file is part of GNUnet.
     (C) 2004, 2005 Christian Grothoff (and other contributing authors)

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
 * @file util/os/osconfig.c
 * @brief functions to read or change the OS configuration
 * @author Nils Durner
 */
#include "platform.h"
#include "gnunet_util_os.h"
#include "gnunet_util_string.h"

/**
 * @brief Enumerate all network interfaces
 * @param callback the callback function
 */
void os_list_network_interfaces(struct GE_Context *ectx,
                                NetworkIfcProcessor proc, void *cls)
{
#ifdef MINGW
  ListNICs(proc, cls);
#else
  char entry[11], *dst;
  FILE *f;

  if(system("ifconfig > /dev/null 2> /dev/null"))
    if(system("/sbin/ifconfig > /dev/null 2> /dev/null") == 0)
      f = popen("/sbin/ifconfig 2> /dev/null", "r");
    else
      f = NULL;
  else
    f = popen("ifconfig 2> /dev/null", "r");
  if(!f)
  {
    GE_LOG_STRERROR_FILE(ectx,
                         GE_USER | GE_ADMIN | GE_BULK | GE_WARNING,
                         "popen", "ifconfig");
    return;
  }

  while(1)
  {
    int i = 0;
    int c = fgetc(f);

    if(c == EOF)
      break;

    dst = entry;

    /* Read interface name until the first space (or colon under OS X) */
    while(c != EOF && c != '\n' &&
#ifdef OSX
          c != ':'
#else
          c != ' '
#endif
          && i < 10)
    {
      *dst++ = c;
      i++;
      c = fgetc(f);
    }
    *dst = 0;

    if((entry[0] != '\0') &&
       (OK != proc(entry, strcmp(entry, "eth0") == 0, cls)))
      break;

    while((c != '\n') && (c != EOF))
      c = fgetc(f);
  }
  pclose(f);
#endif
}

/**
 * @brief Checks if we can start GNUnet automatically
 * @return 1 if yes, 0 otherwise
 */
static int isOSAutostartCapable()
{
#ifdef LINUX
  if(ACCESS("/usr/sbin/update-rc.d", X_OK) == 0)
  {
    /* Debian */
    if(ACCESS("/etc/init.d/", W_OK) == 0)
      return 1;
  }
  return 0;
#else
#ifdef WINDOWS
  return IsWinNT();
#else
  return 0;
#endif
#endif
}

int os_modify_autostart(struct GE_Context *ectx,
                        int testCapability,
                        int doAutoStart,
                        const char *application,
                        const char *username, const char *groupname)
{
  if(testCapability)
  {
    /* TODO: check that user/group/application
       exist! */
    return isOSAutostartCapable();
  }
#ifdef WINDOWS
  if(doAutoStart)
  {
    if(IsWinNT())
    {
      char *err = NULL;
      DWORD dwErr = 0;

      if(username && !strlen(username))
        username = NULL;

      /* Install service */
      switch (InstallAsService(username))
      {
        case 0:
        case 1:
          break;
        case 2:
          if(GetLastError() != ERROR_SERVICE_EXISTS)
            return 1;
        case 3:
          return 2;
        default:
          return -1;
      }

      /* Grant permissions to the GNUnet directory */
      if((!err || dwErr == ERROR_SERVICE_EXISTS) && username)
      {
        char szHome[_MAX_PATH + 1];

        plibc_conv_to_win_path("/", szHome);

        if(!AddPathAccessRights(szHome, username, GENERIC_ALL))
          return 3;
      }
    }
    else
    {
      char szPath[_MAX_PATH + 1];
      HKEY hKey;

      plibc_conv_to_win_path(application, szPath);

      if(RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_EXECUTE, &hKey) == ERROR_SUCCESS)
      {
        if(RegSetValueEx(hKey,
                         "GNUnet",
                         0, REG_SZ, szPath, strlen(szPath)) != ERROR_SUCCESS)
          return 4;

        RegCloseKey(hKey);
      }
      else
        return 4;
    }
  }
  else
  {
    if(IsWinNT())
    {
      switch (UninstallService())
      {
        case 0:
        case 1:
          break;
        case 2:
          return 1;
        case 3:
          return 5;
        case 4:
          return 6;
        default:
          return -1;
      }
    }
    else
    {
      HKEY hKey;

      if(RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
      {
        RegDeleteValue(hKey, "GNUnet");
        RegCloseKey(hKey);
      }
    }
  }
#else
  struct stat buf;

  /* Unix */
  if((ACCESS("/usr/sbin/update-rc.d", X_OK) != 0))
  {
    GE_LOG_STRERROR_FILE(ectx,
                         GE_ERROR | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                         "access", "/usr/sbin/update-rc.d");
    return SYSERR;
  }

  /* Debian */
  if(doAutoStart)
  {
    if(ACCESS(application, X_OK) != 0)
    {
      GE_LOG_STRERROR_FILE(ectx,
                           GE_ERROR | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                           "access", application);
    }
    if(STAT("/etc/init.d/gnunetd", &buf) == -1)
    {
      /* create init file */
      FILE *f = FOPEN("/etc/init.d/gnunetd", "w");
      if(!f)
      {
        GE_LOG_STRERROR_FILE(ectx,
                             GE_ERROR | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                             "fopen", "/etc/init.d/gnunetd");
        return 1;
      }

      fprintf(f,
              "#!/bin/sh\n"
              "#\n"
              "# Automatically created by %s\n"
              "#\n"
              "\n"
              "PIDFILE=/var/run/gnunetd/gnunetd.pid\n"
              "\n"
              "case \"$1\" in\n"
              "	start)\n"
              "		echo -n \"Starting GNUnet: \"\n"
              "		%s\n && echo ok || echo failed\n"
              "		;;\n"
              "	stop)\n"
              "		echo -n \"Stopping GNUnet: \"\n"
              "		kill `cat $PIDFILE`\n && echo ok || echo failed\n"
              "		;;\n"
              "	reload)\n"
              "		echo -n \"Reloading GNUnet: \"\n"
              "		kill -HUP `cat $PIDFILE`\n && echo ok || echo failed\n"
              "		;;\n"
              "	restart|force-reload)\n"
              "		echo \"Restarting GNUnet: gnunetd...\"\n"
              "		$0 stop\n"
              "		sleep 1\n"
              "		$0 start\n"
              "		;;\n"
              "	*)\n"
              "		echo \"Usage: /etc/init.d/gnunetd {start|stop|reload|restart|force-reload}\" >&2\n"
              "		exit 1\n"
              "		;;\n"
              "\n" "esac\n" "exit 0\n", "gnunet-setup", application);
      fclose(f);
      if(0 != CHMOD("/etc/init.d/gnunetd",
                    S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH))
      {
        GE_LOG_STRERROR_FILE(ectx,
                             GE_WARNING | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                             "chmod", "/etc/init.d/gnunetd");
        return SYSERR;
      }
    }
    errno = 0;
    if(-1 == system("/usr/sbin/update-rc.d gnunetd defaults"))
    {
      GE_LOG_STRERROR_FILE(ectx,
                           GE_WARNING | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                           "system", "/usr/sbin/update-rc.d");
      return SYSERR;
    }
    return OK;
  }
  else
  {                             /* REMOVE autostart */
    if((UNLINK("/etc/init.d/gnunetd") == -1) && (errno != ENOENT))
    {
      GE_LOG_STRERROR_FILE(ectx,
                           GE_WARNING | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                           "unlink", "/etc/init.d/gnunetd");
      return SYSERR;
    }
    errno = 0;
    if(-1 != system("/usr/sbin/update-rc.d gnunetd remove"))
    {
      GE_LOG_STRERROR_FILE(ectx,
                           GE_WARNING | GE_USER | GE_ADMIN | GE_IMMEDIATE,
                           "system", "/usr/sbin/update-rc.d");
      return SYSERR;
    }
    return OK;
  }
#endif
  return SYSERR;
}


/* end of osconfig.c */
