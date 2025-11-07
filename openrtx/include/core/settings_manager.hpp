#include "core/settings.h"

#ifndef SETTINGS_MANAGER_HPP
#define SETTINGS_MANAGER_HPP

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This method is responsible for loading settings; it interacts with nvmem and handles settings-specific behaviors.
 * 
 * @param settings_t: pointer to the settings object to have settings read into
 * @returns 0 if sucessful, <0 if unsuccessful
 */
int settings_load(settings_t *out);

/**
 * Save a given settings object to memory
 * 
 * @param settings_t: pointer to the settings object to be saved
 * @returns 0 if successful, <0 if unsuccessful
 * 
 */
int settings_save(const settings_t *in);

#ifdef __cplusplus
}
#endif

#endif /* SETTIGNS_MANAGER_HPP*/
