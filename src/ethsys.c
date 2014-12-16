/**
 * collectd - src/ethsys.c
 * Copyright (C) 2014	    Quentin Kaiser
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Quentin Kaiser <quentin at gremwell.com>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"

#include "utils_complain.h"

#if HAVE_MACH_MACH_TYPES_H
#  include <mach/mach_types.h>
#endif
#if HAVE_MACH_MACH_INIT_H
#  include <mach/mach_init.h>
#endif
#if HAVE_MACH_MACH_ERROR_H
#  include <mach/mach_error.h>
#endif

#if KERNEL_LINUX
# define SYSFS_PATH "/sys/class/net"
# define SYSFS_FACTOR 0.000001
#endif /* KERNEL_LINUX */



static char **interfaces = NULL;
static size_t interfaces_num = 0;


static int ethsys_add_interface (const oconfig_item_t *ci) /* {{{ */
{
  char **tmp;
  int status;

  tmp = realloc (interfaces,
      sizeof (*interfaces) * (interfaces_num + 1));
  if (tmp == NULL)
    return (-1);
  interfaces = tmp;
  interfaces[interfaces_num] = NULL;

  status = cf_util_get_string (ci, interfaces + interfaces_num);
  if (status != 0)
    return (status);

  interfaces_num++;
  INFO("ethsys plugin: Registered interface %s",
      interfaces[interfaces_num - 1]);

  return (0);
} /* }}} int ethsys_add_interface */


/* Reads a file which contains only a number (and optionally a trailing
 * newline) and parses that number. */
static int sysfs_file_to_buffer(char const *dir,
		char const *device,
		char const *basename,
		char *buffer, size_t buffer_size)
{
	int status;
	FILE *fp;
	char filename[PATH_MAX];

	ssnprintf (filename, sizeof (filename), "%s/%s/%s",
			dir, device, basename);

	/* No file isn't the end of the world -- not every system will be
	 * reporting the same set of statistics */
	if (access (filename, R_OK) != 0)
		return ENOENT;

	fp = fopen (filename, "r");
	if (fp == NULL)
	{
		status = errno;
		if (status != ENOENT)
		{
			char errbuf[1024];
			WARNING ("ethsys plugin: fopen (%s) failed: %s", filename,
					sstrerror (status, errbuf, sizeof (errbuf)));
		}
		return status;
	}

	if (fgets (buffer, buffer_size, fp) == NULL)
	{
		char errbuf[1024];
		status = errno;
		WARNING ("ethsys plugin: fgets failed: %s",
				sstrerror (status, errbuf, sizeof (errbuf)));
		fclose (fp);
		return status;
	}

	strstripnewline (buffer);

	fclose (fp);
	return 0;
} /* }}} int sysfs_file_to_buffer */


/* Reads a file which contains only a number (and optionally a trailing
 * newline) and parses that number. */
static int sysfs_file_to_gauge(char const *dir, /* {{{ */
                char const *device,
                char const *basename, gauge_t *ret_value)
{
        int status;
        char buffer[32] = "";

        status = sysfs_file_to_buffer (dir, device, basename, buffer, sizeof (buffer));
        if (status != 0)
                return (status);

        return (strtogauge (buffer, ret_value));
} /* }}} sysfs_file_to_gauge */

static void ethsys_submit_one (char *plugin_instance,
                const char *type, const char *type_instance,
                gauge_t value)
{
        value_t values[1];
        value_list_t vl = VALUE_LIST_INIT;

        values[0].gauge = value;

        vl.values = values;
        vl.values_len = 1;
        sstrncpy (vl.host, hostname_g, sizeof (vl.host));
        sstrncpy (vl.plugin, "ethsys", sizeof (vl.plugin));
        if (plugin_instance != NULL)
                sstrncpy (vl.plugin_instance, plugin_instance,
                                sizeof (vl.plugin_instance));
        sstrncpy (vl.type, type, sizeof (vl.type));
        if (type_instance != NULL)
                sstrncpy (vl.type_instance, type_instance,
                                sizeof (vl.type_instance));

        plugin_dispatch_values (&vl);
} /* void ethsys_submit_one */

static int ethsys_read_interface (char *device)
{
	char buffer[32] = "";
	gauge_t* value = malloc(sizeof(gauge_t));
	int status;	

	status = sysfs_file_to_gauge(SYSFS_PATH, device, "speed", value);
	if(status != 0)
		return status;
	ethsys_submit_one (device, "link", "speed", *value);
	DEBUG("Read speed sysfs : %d", value);	

	status = sysfs_file_to_buffer(SYSFS_PATH, device, "duplex", buffer, sizeof(buffer));
	if(status != 0)
		return status;
	if(!strcmp(buffer, "half")){
		ethsys_submit_one (device, "link", "duplex", ((gauge_t)0.5));
	}
	else if(!strcmp(buffer, "full")){
			ethsys_submit_one (device, "link", "duplex", ((gauge_t)1));
	}	
	else{
		ethsys_submit_one (device, "link", "duplex", ((gauge_t)0));
	}
	DEBUG("Read speed sysfs : %s", buffer);	
	memset(buffer, '\0', sizeof(buffer));
	
	status = sysfs_file_to_gauge(SYSFS_PATH, device, "carrier", value);	
	if(status != 0)
		return status;
	ethsys_submit_one (device, "link", "carrier", *value);
	DEBUG("Read carrier sysfs : %d", value);	


	FILE *fp;
  	char autonego[5];

	/* Open the command for reading. */
	fp = popen("ethtool eth0 2>/dev/null | grep \"Advertised auto\" | awk '{FS=\":\"} {print $3}'", "r");
	if (fp == NULL) {
	  DEBUG("Failed to run command\n" );
	}
	if(fgets(autonego, sizeof(autonego)-1, fp)){
	  if(!strncmp(autonego, "Yes", 3)){
	    ethsys_submit_one (device, "link", "autonegotiation", (gauge_t)1);
  	  }else{
  	    ethsys_submit_one (device, "link", "autonegotiation", (gauge_t)0);
	  }
	}
	/* close */
	pclose(fp);

	return(1);
}

static int ethsys_read (void) /* {{{ */
{
	
	size_t i;
	for (i = 0; i < interfaces_num; i++)
    		ethsys_read_interface (interfaces[i]);
	return 0;
} /* }}} int ethsys_read */

static int ethsys_config (oconfig_item_t *ci)
{
	int i;

	for (i = 0; i < ci->children_num; i++)
	{
		oconfig_item_t *child = ci->children + i;

		if (strcasecmp ("Interface", child->key) == 0)
			ethsys_add_interface (child);
		else
			WARNING ("battery plugin: Ignoring unknown "
					"configuration option \"%s\".",
					child->key);
	}

	return (0);
} /* }}} int battery_config */

void module_register (void)
{
	plugin_register_complex_config ("ethsys", ethsys_config);
	plugin_register_read ("ethsys", ethsys_read);
} /* void module_register */
