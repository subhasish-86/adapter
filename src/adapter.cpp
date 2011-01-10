/*
* Copyright (c) 2008, AMT – The Association For Manufacturing Technology (“AMT”)
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of the AMT nor the
*       names of its contributors may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* DISCLAIMER OF WARRANTY. ALL MTCONNECT MATERIALS AND SPECIFICATIONS PROVIDED
* BY AMT, MTCONNECT OR ANY PARTICIPANT TO YOU OR ANY PARTY ARE PROVIDED "AS IS"
* AND WITHOUT ANY WARRANTY OF ANY KIND. AMT, MTCONNECT, AND EACH OF THEIR
* RESPECTIVE MEMBERS, OFFICERS, DIRECTORS, AFFILIATES, SPONSORS, AND AGENTS
* (COLLECTIVELY, THE "AMT PARTIES") AND PARTICIPANTS MAKE NO REPRESENTATION OR
* WARRANTY OF ANY KIND WHATSOEVER RELATING TO THESE MATERIALS, INCLUDING, WITHOUT
* LIMITATION, ANY EXPRESS OR IMPLIED WARRANTY OF NONINFRINGEMENT,
* MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE. 

* LIMITATION OF LIABILITY. IN NO EVENT SHALL AMT, MTCONNECT, ANY OTHER AMT
* PARTY, OR ANY PARTICIPANT BE LIABLE FOR THE COST OF PROCURING SUBSTITUTE GOODS
* OR SERVICES, LOST PROFITS, LOSS OF USE, LOSS OF DATA OR ANY INCIDENTAL,
* CONSEQUENTIAL, INDIRECT, SPECIAL OR PUNITIVE DAMAGES OR OTHER DIRECT DAMAGES,
* WHETHER UNDER CONTRACT, TORT, WARRANTY OR OTHERWISE, ARISING IN ANY WAY OUT OF
* THIS AGREEMENT, USE OR INABILITY TO USE MTCONNECT MATERIALS, WHETHER OR NOT
* SUCH PARTY HAD ADVANCE NOTICE OF THE POSSIBILITY OF SUCH DAMAGES.
*/

#include "internal.hpp"
#include "adapter.hpp"
#include "device_datum.hpp"
#include "logger.hpp"

Adapter::Adapter(int aPort, int aScanDelay)
 : mNumDeviceData(0)
{
  mScanDelay = aScanDelay;
  mServer = 0;
  mPort = aPort;
  mHeartbeatFrequency = 10000;
  mMaxDatum = INITIAL_MAX_DEVICE_DATA;
  mRunning = false;
  mDeviceData = new DeviceDatum*[mMaxDatum];
}

Adapter::~Adapter()
{
  mRunning = false;
  if (mServer)
    delete mServer;

  delete[] mDeviceData;
}

/* Add a data value to the list of data values */
void Adapter::addDatum(DeviceDatum &aValue)
{
  if (mNumDeviceData >= mMaxDatum)
  {
    DeviceDatum** devData = new DeviceDatum*[mMaxDatum * 2];
    memcpy(devData, mDeviceData, sizeof(DeviceDatum*) * mMaxDatum);
    delete[] mDeviceData;
    
    mDeviceData = devData;
    mMaxDatum *= 2;
  }
  
  mDeviceData[mNumDeviceData++] = &aValue;
  mDeviceData[mNumDeviceData] = 0;
}

void Adapter::sleepMs(int aMs)
{
#ifndef WIN32
    usleep(aMs * 1000);
#else
    Sleep(aMs);
#endif
  
}

#if defined(WIN32) && defined(THREADED)

static int ServerThread(void *aArg)
{
  Adapter *adapter = (Adapter*) aArg;
  adapter->serverThread();
  return 0;
}

bool Adapter::startServerThread() 
{
  if (gLogger == NULL) gLogger = new Logger();
  
  mServer = new Server(mPort, mHeartbeatFrequency);  
  mRunning = true;

  mServerThread = CreateThread(0, 0, (LPTHREAD_START_ROUTINE) &ServerThread, this, 0, 0);
  if (mServerThread == NULL) {
    fprintf(stderr, "Cannot create server thread");
    delete mServer;
    return false;
  } else {
    CloseHandle(mServerThread);
  }
  
  return true;
}

/* Poll every second for activity. We could do blocking but it complicates 
 * list management and locking. May refactor this when we have more time.
 */
void Adapter::serverThread()
{
  while (mRunning) {
    connectToClients();
    readFromClients();
    Sleep(1000);
  }
}

#endif

/*
 * Reads from clients, either blocking or polling.
 */
void Adapter::readFromClients() 
{
  mServer->readFromClients();
}

/*
 * Checks for new client connections, either blocking or polling.
 */

void Adapter::connectToClients()
{
  /* Check if we have any new clients */
  Client **clients = mServer->connectToClients();
  if (clients != 0)
  {
    for (int i = 0; clients[i] != 0; i++)
    {
      /* If there are any new clients, send them the initial values for all the 
       * data values */
      sendInitialData(clients[i]);
    }
  }  
}
 
/*  
 * This is the main loop of the application. Once the server is started and 
 * the application runs forever until it is killed. The process follows this flow:
 *    See if any new clients have arrived.
 *      If new clients are present, sent the initial values to them.
 *    Read from the clients any data that is currently in the socket to keep anyone from
 *      blocking.
 *    If we have at least one client:
 *      Timestamp the buffer
 *      gather data from the API
 *      send the data values that have changed to the clients
 *    Sleep for a few millies and repeat.
 */
void Adapter::startServer()
{  
  if (gLogger == NULL) gLogger = new Logger();
  
  mServer = new Server(mPort, mHeartbeatFrequency);  
  mRunning = true;
  
  /* Process untill stopped */
  while (mRunning) 
  {
    /* Check if we have any new clients */
    connectToClients();
    
    /* Read and all data from the clients */
    readFromClients();
    
    /* Don't bother getting data if we don't have anyone to read it */
    if (mServer->numClients() > 0)
    {
      begin();
      mBuffer.timestamp();
      gatherDeviceData();
      sendChangedData();
      mBuffer.reset();
      cleanup();
    }
    else if (mServer->hasClients())
    {
      clientsDisconnected();
    }
    
    sleepMs(mScanDelay);
  }
  
  delete mServer;
  mServer = NULL;
}

void Adapter::begin()
{
  for (int i = 0; i < mNumDeviceData; i++)
  {
    DeviceDatum *value = mDeviceData[i];
    value->begin();
  }
}

void Adapter::cleanup()
{
  for (int i = 0; i < mNumDeviceData; i++)
  {
    DeviceDatum *value = mDeviceData[i];
    value->cleanup();
  }
}


void Adapter::stopServer()
{    
  mRunning = false;
}

/* Send a single value to the buffer. */
void Adapter::sendDatum(DeviceDatum *aValue)
{
  if (aValue->requiresFlush())
    sendBuffer();
  aValue->append(mBuffer);
  if (aValue->requiresFlush())
    sendBuffer();
}

/* Send the buffer to the clients. Only sends if there is something in the buffer. */
void Adapter::sendBuffer()
{
  if (mServer != 0 && mBuffer.length() > 0)
  {
    mBuffer.append("\n");
    mServer->sendToClients(mBuffer);
    mBuffer.reset();  
  }
}

/* Send the initial values to a client */
void Adapter::sendInitialData(Client *aClient)
{
  mDisableFlush = true;
  mBuffer.timestamp();
  gatherDeviceData();
  
  for (int i = 0; i < mNumDeviceData; i++)
  {
    DeviceDatum *value = mDeviceData[i];
    if (value->hasInitialValue())
      sendDatum(value);
  }
  sendBuffer();
  mDisableFlush = false;
}

/* Send the values that have changed to the clients */
void Adapter::sendChangedData()
{
  for (int i = 0; i < mNumDeviceData; i++)
  {
    DeviceDatum *value = mDeviceData[i];
    if (value->changed())
      sendDatum(value);
  }  
  sendBuffer();
}

void Adapter::flush()
{
  if (!mDisableFlush)
  {
    sendChangedData();
    mBuffer.reset();
    mBuffer.timestamp();
  }
}

void Adapter::clientsDisconnected()
{
  /* Do nothing for now ... */
  printf("All clients have disconnected\n");
}

void Adapter::unavailable()
{
  for (int i = 0; i < mNumDeviceData; i++)
  {
    DeviceDatum *value = mDeviceData[i];
    value->unavailable();
  }
  flush();
}

