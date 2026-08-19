#include "rns_plugin.h"
#include <assert.h>
#include <dlfcn.h>
#include <string.h>
int main(int argc,char **argv) {
    const rns_plugin_api_t *(*get_api)(void); const rns_plugin_api_t *api; void *library;
    assert(argc==2); library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); assert(library);
    *(void **)(&get_api)=dlsym(library,"rns_plugin_get_api"); assert(get_api);
    api=get_api(); assert(api && api->abi_major==RNS_PLUGIN_ABI_MAJOR);
    assert(api->info && api->info->name.len && api->create && api->send && api->destroy);
    assert(memcmp(api->info->name.data,"SX1262 RNode",api->info->name.len)==0);
    dlclose(library); return 0;
}
