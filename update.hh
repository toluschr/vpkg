#ifndef VPKG_UPDATE_HH_
#define VPKG_UPDATE_HH_

#include <stdbool.h>
#include <xbps.h>

#include "config.hh"
#include "util.h"

int vpkg_do_update(vpkg_context *ctx, int argc, char **argv);

#endif // VPKG_UPDATE_HH_
