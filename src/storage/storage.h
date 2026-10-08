#pragma once

#include <Arduino.h>
#include <SD.h>

bool storageInit();

File storageGet(const char* filename);
File storageOpen(const char* filename);

void storageClose(File& file);
void storageFlush(File& file);

bool storageRemove(const char* filename);
bool storageExists(const char* filename);