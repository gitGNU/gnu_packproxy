/*  Copyright (C) 2008, 2009 Ben Asselstine

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Library General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.
*/
#include <stdlib.h>
#include <error.h>
#include <string.h>
#include <argp.h>
#include "opts.h"

#define FULL_VERSION PACKAGE_NAME " " PACKAGE_VERSION

const char *argp_program_version = FULL_VERSION "\n"
"This is free software: you are free to change and redistribute it.\n"
"There is NO WARRANTY, to the extent permitted by law.";

const char *argp_program_bug_address = "<" PACKAGE_BUGREPORT ">";
static char doc[] = "A bandwidth-saving http proxy.";
static char args_doc[] = "";

#define OAO OPTION_ARG_OPTIONAL
#define OH OPTION_HIDDEN

struct arguments_t arguments;
static struct argp_option options[] = 
{
    { "verbose", OPT_VERBOSE, "NUM", OAO, "Show more or less information " 
      "(Default " DEFAULT_VERBOSE_VALUE ")", 1 },
    { "debug", OPT_DEBUG, "VALUE", OAO | OH, 
      "Run into debugging mode (Default " 
	DEFAULT_DEBUG_VALUE ")", 1 },
    { "port", OPT_PORT, "VALUE", 0, 
      "Listen for connections on this internet port (Default " 
	DEFAULT_PORT_VALUE ")", 1 },
    { 0 }
};

static void
init_options (struct ziproxy_ng_options_t *ziproxy_ng)
{
  ziproxy_ng->verbose = -1;
  ziproxy_ng->debug = -1;
  ziproxy_ng->port = -1;
  return;
}

static error_t 
parse_opt (int key, char *arg, struct argp_state *state) 
{
  char *end = NULL;
  struct arguments_t *arguments = (struct arguments_t *) state->input;

  switch (key) 
    { 
    case OPT_PORT:
      arguments->ziproxy_ng.port = strtoul (arg, &end, 0);
      if ((end == NULL) || (end == arg))
	{
	  argp_error (state, 
		      "the argument to --port isn't a number.");
	  return EINVAL;
	}
      if (arguments->ziproxy_ng.port <= 0 || arguments->ziproxy_ng.port > 32768)
	{
	  argp_error (state, 
		      "the argument to --port isn't between 1 and 32768.");
	  return EINVAL;
	}
      break;
    case OPT_DEBUG:
      if (arg)
	{
	  arguments->ziproxy_ng.debug = strtoul (arg, &end, 0);
	  if ((end == NULL) || (end == arg))
	    {
	      argp_error (state, 
			  "the argument to --debug isn't a number.");
	      return EINVAL;
	    }
	}
      else
	arguments->ziproxy_ng.debug = 1;
      break;
    case OPT_VERBOSE:
      if (arg)
	{
	  arguments->ziproxy_ng.verbose = strtoul (arg, &end, 0);
	  if ((end == NULL) || (end == arg))
	    {
	      argp_error (state, "the argument to --verbose isn't a number.");
	      return EINVAL;
	    }
	}
      else
	{
	  if (arguments->ziproxy_ng.verbose == -1)
	    arguments->ziproxy_ng.verbose = 1;
	  else
	    arguments->ziproxy_ng.verbose++;
	}
      break;
    case ARGP_KEY_INIT:
      init_options (&arguments->ziproxy_ng);
      break;
    case ARGP_KEY_ARG:
      break;
    case ARGP_KEY_END:
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}


static const struct argp_child child_parsers[] = 
{
    {0}
};

struct argp argp = { options, parse_opt, args_doc, doc, child_parsers }; 

static void 
set_default_options (struct ziproxy_ng_options_t *ziproxy_ng)
{
  if (ziproxy_ng->verbose == -1)
    ziproxy_ng->verbose = atoi (DEFAULT_VERBOSE_VALUE);
  if (ziproxy_ng->debug == -1)
    ziproxy_ng->debug = atoi (DEFAULT_DEBUG_VALUE);
  if (ziproxy_ng->port == -1)
    ziproxy_ng->port = atoi (DEFAULT_PORT_VALUE);
  return;
}

void 
parse_opts (int argc, char **argv, struct arguments_t *arguments)
{
  int retval;
  setenv ("ARGP_HELP_FMT", "no-dup-args-note", 1);
  retval = argp_parse (&argp, argc, argv, 0, 0, arguments); 
  if (retval < 0)
    argp_help (&argp, stdout, ARGP_HELP_EXIT_ERR|ARGP_HELP_SEE,PACKAGE_NAME);
  set_default_options (&arguments->ziproxy_ng);

  return;
}
