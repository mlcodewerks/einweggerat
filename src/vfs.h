#pragma once
#include "common.h"
// Native Windows VFS v4. Read-only regular files are memory mapped.
const retro_vfs_interface *windows_vfs();
bool vfs_environment(void *data);
