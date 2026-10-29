/*
 * obs-neutral-balance - manual white balance correction filter for OBS Studio
 * Copyright (C) 2026 obs-neutral-balance contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <obs-module.h>

#include "neutral-balance-filter.h"

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("obs-neutral-balance", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Measure-and-correct manual white balance, darktable-style";
}

bool obs_module_load(void)
{
	obs_register_source(&neutral_balance_filter_info);
	blog(LOG_INFO, "[neutral-balance] plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[neutral-balance] plugin unloaded");
}
