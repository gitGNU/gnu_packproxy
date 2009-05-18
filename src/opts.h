/*  Copyright (C) 2008 Ben Asselstine

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
#ifndef OPTS_H
#define OPTS_H 1

// the options
enum ziproxy_ng_command_line_options_t
{
  OPT_DEBUG = -123,
  OPT_VERBOSE = 'v',
  OPT_PORT = 'p',
};

// types
struct ziproxy_ng_options_t
{
  int verbose;
  int debug;
  int port;
};

struct arguments_t 
{
  struct ziproxy_ng_options_t ziproxy_ng;
};

// external prototypes
void parse_opts (int argc, char **argv, struct arguments_t *arguments);

#define DEFAULT_VERBOSE_VALUE "0"
#define DEFAULT_DEBUG_VALUE "0"
#define DEFAULT_PORT_VALUE "7001"

#endif
