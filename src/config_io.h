#pragma once

void load_destinations();
void save_destinations();

/** Same as load/save but with an explicit file path (used by tests and for alternate locations). */
void load_destinations_from_file(const char *filename);
void save_destinations_to_file(const char *filename);

void sync_default_destination_from_obs();
