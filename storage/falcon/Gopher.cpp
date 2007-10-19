/* Copyright (C) 2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "Engine.h"
#include "Gopher.h"
#include "SerialLog.h"
#include "Sync.h"
#include "Thread.h"
#include "Threads.h"
#include "SerialLogTransaction.h"
#include "Database.h"

Gopher::Gopher(SerialLog *serialLog)
{
	log = serialLog;
}

Gopher::~Gopher(void)
{
}

void Gopher::gopherThread(void* arg)
{
	((Gopher*) arg)->gopherThread();
}

void Gopher::gopherThread(void)
{
	Sync deadMan(&syncGopher, "Gopher::gopherThread");
	deadMan.lock(Exclusive);
	workerThread = Thread::getThread("Gopher::gopherThread");
	active = true;
	Sync sync (&log->pending.syncObject, "Gopher::gopherThread pending");
	Sync updateBlocker(&log->syncUpdateStall, "Gopher::gopherThread blocker");
	
	while (!workerThread->shutdownInProgress && !log->finishing)
		{
		if (!log->pending.first || !log->pending.first->isRipe())
			{
			if (log->blocking)
				{
				log->blocking = false;
				updateBlocker.unlock();
				}

			workerThread->sleep();
			
			continue;
			}
		
		SerialLogTransaction *transaction = log->pending.first;
		transaction->doAction();
		sync.lock(Exclusive);
		log->pending.remove(transaction);
		log->inactions.append(transaction);
		
		if (log->pending.count > log->maxTransactions && !log->blocking)
			{
			log->blocking = true;
			updateBlocker.lock(Exclusive);
			//Log::debug("Transaction backlog limit exceed, freezing updates\n");
			++log->backlogStalls;
			}

		sync.unlock();
		}

	active = false;
	workerThread = NULL;
}

void Gopher::start(void)
{
	log->database->threads->start("SerialLog::start", gopherThread, this);
}

void Gopher::shutdown(void)
{
	if (workerThread)
		workerThread->shutdown();
}
