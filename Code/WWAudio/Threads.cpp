/*
 * 	Command & Conquer Renegade(tm)
 * 	Copyright 2025 Electronic Arts Inc.
 * 	Copyright 2025 CnC: Rebel Developers.
 *
 * 	This program is free software: you can redistribute it and/or modify
 * 	it under the terms of the GNU General Public License as published by
 * 	the Free Software Foundation, either version 3 of the License, or
 * 	(at your option) any later version.
 *
 * 	This program is distributed in the hope that it will be useful,
 * 	but WITHOUT ANY WARRANTY; without even the implied warranty of
 * 	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * 	GNU General Public License for more details.
 *
 * 	You should have received a copy of the GNU General Public License
 * 	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WWAudio                                                      *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/WWAudio/Threads.cpp $Modtime:: 7/17/99 3:32p $*
 *                                                                                             *
 *                    $Revision:: 9                                                           $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <process.h>

#include "Threads.h"
#include "refcount.h"
#include "Utils.h"
#include "systimer.h"

///////////////////////////////////////////////////////////////////////////////////////////
//	Static member initialization
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::DELAYED_RELEASE_INFO *WWAudioThreadsClass::m_ReleaseListHead = nullptr;
std::recursive_mutex WWAudioThreadsClass::m_ListMutex;
HANDLE WWAudioThreadsClass::m_hDelayedReleaseThread = (HANDLE)-1;
HANDLE WWAudioThreadsClass::m_hDelayedReleaseEvent = (HANDLE)-1;
std::recursive_mutex WWAudioThreadsClass::m_CriticalSection;
bool WWAudioThreadsClass::m_IsFlushing = false;

///////////////////////////////////////////////////////////////////////////////////////////
//
//	WWAudioThreadsClass
//
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::WWAudioThreadsClass() = default;

///////////////////////////////////////////////////////////////////////////////////////////
//
//	~WWAudioThreadsClass
//
///////////////////////////////////////////////////////////////////////////////////////////
WWAudioThreadsClass::~WWAudioThreadsClass() = default;

///////////////////////////////////////////////////////////////////////////////////////////
//
//	Create_Delayed_Release_Thread
//
///////////////////////////////////////////////////////////////////////////////////////////
// The delayed releases ran on a thread of their own, which released sounds
// (taking the audio lock) while holding the list's lock, and the main thread
// takes the two the other way round (stopping a sound queues its buffer
// here): a deadlock, and reference counts changed from two threads. They
// run on the main thread now, from the audio frame update.
HANDLE WWAudioThreadsClass::Create_Delayed_Release_Thread(LPVOID) { return (HANDLE)-1; }

void WWAudioThreadsClass::End_Delayed_Release_Thread(DWORD) { Flush_Delayed_Release_Objects(); }

void WWAudioThreadsClass::Add_Delayed_Release_Object(RefCountClass *object, DWORD delay) {
  if (m_IsFlushing) {
    REF_PTR_RELEASE(object);
  } else {
    {
      std::lock_guard lock(m_ListMutex);

      //
      //	Create a new delay-information structure and
      //	add it to our list
      //
      DELAYED_RELEASE_INFO *info = new DELAYED_RELEASE_INFO;
      info->object = object;
      info->time = TIMEGETTIME() + delay;
      info->next = m_ReleaseListHead;
      info->prev = nullptr;

      if (info->next != nullptr) {
        info->next->prev = info;
      }

      m_ReleaseListHead = info;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////
//
//	Flush_Delayed_Release_Objects
//
///////////////////////////////////////////////////////////////////////////////////////////
void WWAudioThreadsClass::Flush_Delayed_Release_Objects() {
  std::lock_guard lock(m_ListMutex);
  m_IsFlushing = true;

  //
  //	Loop through all the objects in our delay list, and
  // free them now.
  //
  DELAYED_RELEASE_INFO *info = nullptr;
  DELAYED_RELEASE_INFO *next = nullptr;
  for (info = m_ReleaseListHead; info != nullptr; info = next) {
    next = info->next;

    //
    //	Free the object
    //
    REF_PTR_RELEASE(info->object);
    SAFE_DELETE(info);
  }

  m_ReleaseListHead = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////
//
//	Release_Due_Objects
//
///////////////////////////////////////////////////////////////////////////////////////////
void WWAudioThreadsClass::Release_Due_Objects() {
  // Unlinked under the lock, released after it: a release may queue more.
  DELAYED_RELEASE_INFO *due = nullptr;
  {
    std::lock_guard lock(m_ListMutex);
    DWORD current_time = TIMEGETTIME();
    DELAYED_RELEASE_INFO *next = nullptr;
    for (DELAYED_RELEASE_INFO *curr = m_ReleaseListHead; curr != nullptr; curr = next) {
      next = curr->next;
      if ((int)(current_time - curr->time) >= 0) {
        if (curr == m_ReleaseListHead) {
          m_ReleaseListHead = next;
        }
        if (curr->prev != nullptr) {
          curr->prev->next = next;
        }
        if (next != nullptr) {
          next->prev = curr->prev;
        }
        curr->next = due;
        due = curr;
      }
    }
  }
  while (due != nullptr) {
    DELAYED_RELEASE_INFO *next = due->next;
    REF_PTR_RELEASE(due->object);
    SAFE_DELETE(due);
    due = next;
  }
}

/*
///////////////////////////////////////////////////////////////////////////////////////////
//
//	Begin_Modify_List
//
///////////////////////////////////////////////////////////////////////////////////////////
bool
WWAudioThreadsClass::Begin_Modify_List ()
{
        bool retval = false;

        //
        //	Wait for up to one second to modify the list object
        //
        if (m_ListMutex != nullptr) {
                retval = (::WaitForSingleObject (m_ListMutex, 1000) == WAIT_OBJECT_0);
                WWASSERT (retval);
        }

        return retval;
}


///////////////////////////////////////////////////////////////////////////////////////////
//
//	End_Modify_List
//
///////////////////////////////////////////////////////////////////////////////////////////
void
WWAudioThreadsClass::End_Modify_List ()
{
        //
        //	Release this thread's hold on the mutex object.
        //
        if (m_ListMutex != nullptr) {
                ::ReleaseMutex (m_ListMutex);
        }

        return ;
}
*/