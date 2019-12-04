/* Copyright (c) 2016-2017 Wojciech Owczarek,
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   clockdriver.c
 * @date   Sat Jan 9 16:14:10 2015
 *
 * @brief  Initialisation and control code for the clock driver
 *
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>

#include <libcck/cck.h>
#include <libcck/cck_logger.h>
#include <libcck/cck_utils.h>
#include <libcck/clockdriver.h>

#define THIS_COMPONENT "clock: "

#define PREFMST_REFNAME "EXTSYNC"

/* linked list - so that we can control all registered objects centrally */
LL_ROOT(ClockDriver);

static ClockDriver* _systemClock = NULL;
static ClockDriver* _bestClock = NULL;
static ClockDriver *_masterClock = NULL;

// TODO: take care of this
//static int _updateInterval = CLOCKDRIVER_UPDATE_INTERVAL;
static int _syncInterval = 1.0 / (CLOCKDRIVER_SYNC_RATE + 0.0);


static const char *clockDriverNames[] = {

    #define CCK_ALL_IMPL
    #define CCK_REGISTER_IMPL(typeenum, typesuffix, textname) \
	[typeenum] = textname,

    #include "clockdriver.def"

    [CLOCKDRIVER_MAX] = "nosuchtype"

};

/* inherited methods */

static void setState(ClockDriver *, ClockState, ClockStateReason);
static void processUpdate(ClockDriver *);
static void touchClock(ClockDriver *driver);
static bool healthCheck(ClockDriver *);
static void setReference(ClockDriver *, ClockDriver *);
static void setExternalReference(ClockDriver *, const char*, int);
static void restoreFrequency (ClockDriver *);
static void storeFrequency (ClockDriver *);
static bool stepTime (ClockDriver* , CckTimestamp*, bool);
static bool adjustFrequency (ClockDriver *, double, double);
static bool syncClock (ClockDriver*, double);
static bool syncClockExternal (ClockDriver*, CckTimestamp, double);
static void putInfoLine(ClockDriver*, char*, int);
static void putStatsLine(ClockDriver*, char*, int);


/* inherited methods end */

static ClockDriver* compareClockDriver(ClockDriver *, ClockDriver *);
static void findBestClock();
static bool disciplineClock(ClockDriver *, CckTimestamp, double);
static bool estimateFrequency(ClockDriver *, double tau);
static void setFrequencyEstimate(ClockDriver *);
static void resetClockAge(ClockDriver *driver);
static bool filterClock(ClockDriver *driver, double tau); /* filter the current refOffset */

ClockDriver *
createClockDriver(int driverType, const char *name)
{

    ClockDriver *clockDriver = NULL;

    if(getClockDriverByName(name) != NULL) {
	CCK_ERROR(THIS_COMPONENT"Cannot create clock driver %s: clock driver with this name already exists\n", name);
	return NULL;
    }

    CCKCALLOC(clockDriver, sizeof(ClockDriver));

    if(!setupClockDriver(clockDriver, driverType, name)) {
	if(clockDriver != NULL) {
	    free(clockDriver);
	}
	return NULL;
    } else {
	/* maintain the linked list */
	LL_APPEND_STATIC(clockDriver);
    }

    clockDriver->refClass = RC_NONE;
    clockDriver->distance = 255;

    clockDriver->_filter = NULL;
    clockDriver->_madFilter = NULL;

    return clockDriver;

}

bool
setupClockDriver(ClockDriver* clockDriver, int driverType, const char *name)
{

    bool found = false;
    bool setup = false;

    clockDriver->type = driverType;
    strncpy(clockDriver->name, name, CCK_COMPONENT_NAME_MAX);

    /* reset callbacks */
    memset(&clockDriver->callbacks, 0, sizeof(clockDriver->callbacks));

    /* inherited methods - implementation may wish to override them,
     * or even preserve these pointers in its private data and call both
     */

    clockDriver->setState = setState;
    clockDriver->processUpdate = processUpdate;
//    clockDriver->pushConfig = pushConfig;
    clockDriver->healthCheck = healthCheck;

    clockDriver->stepTime = stepTime;
    clockDriver->setReference = setReference;
    clockDriver->setExternalReference = setExternalReference;

    clockDriver->storeFrequency = storeFrequency;
    clockDriver->restoreFrequency = restoreFrequency;
    clockDriver->adjustFrequency = adjustFrequency;

    clockDriver->syncClock = syncClock;
    clockDriver->syncClockExternal = syncClockExternal;

    clockDriver->touchClock = touchClock;

    clockDriver->putStatsLine = putStatsLine;
    clockDriver->putInfoLine = putInfoLine;

    /* callback placeholders */
    clockDriver->_vendorInit = clockDriverDummyCallback;
    clockDriver->_vendorShutdown = clockDriverDummyCallback;
    clockDriver->_vendorHealthCheck = clockDriverDummyCallback;

    /* inherited methods end */

    /* these macros call the setup functions for existing clock drivers */

    #define CCK_REGISTER_IMPL(typeenum, typesuffix, textname) \
	if(driverType==typeenum) { \
	    setup = _setupClockDriver_##typesuffix(clockDriver);\
	    found = true;\
	}
    #include "clockdriver.def"

    if(!found) {
	CCK_ERROR(THIS_COMPONENT"setupClockDriver(): Requested unknown clock driver type: 0x%02x\n", driverType);
    } else if(!setup) {
	return false;
    } else {
	clockDriver->getTimeMonotonic(clockDriver, &clockDriver->_initTime);
	clockDriver->getTimeMonotonic(clockDriver, &clockDriver->_lastUpdate);
	CCK_DBG(THIS_COMPONENT"Created new clock driver type %d name %s serial %d\n", driverType, name, clockDriver->_serial);
    }

    clockDriver->state = CS_INIT;
    setupPIservo(&clockDriver->servo);
    clockDriver->servo.controller = clockDriver;

    return found;

}

void
freeClockDriver(ClockDriver** clockDriver)
{

    ClockDriver *pdriver = *clockDriver;
    ClockDriver *cd;

    if(*clockDriver == NULL) {
	return;
    }

    /* remove reference from all clocks referencing this clock */
    LL_FOREACH_STATIC(cd) {
	if(cd->refClock == pdriver) {
	    cd->setReference(cd, NULL);
	}
    }

    if(pdriver->_init) {
	pdriver->shutdown(pdriver);
    }

    /* maintain the linked list */
    LL_REMOVE_STATIC(pdriver);


    if(pdriver->_privateData != NULL) {
	free(pdriver->_privateData);
    }

    if(pdriver->_privateConfig != NULL) {
	free(pdriver->_privateConfig);
    }

    CCK_DBG(THIS_COMPONENT"Deleted clock driver type %d name %s serial %d\n", pdriver->type, pdriver->name, pdriver->_serial);

    if(pdriver == _systemClock) {
	_systemClock = NULL;
    }

    freeDoubleMovingStatFilter(&pdriver->_filter);
    freeDoubleMovingStatFilter(&pdriver->_madFilter);

    free(*clockDriver);

    *clockDriver = NULL;

};

ClockDriver*
getSystemClock() {

    if(_systemClock != NULL) {
	return _systemClock;
    }

    _systemClock = createClockDriver(CLOCKDRIVER_UNIX, SYSTEM_CLOCK_NAME);

    if(_systemClock == NULL) {
	CCK_CRITICAL(THIS_COMPONENT"Could not start system clock driver, cannot continue\n");
	exit(1);
    }

    _systemClock->init(_systemClock, NULL);

    return _systemClock;

}

void
shutdownClockDrivers() {

	ClockDriver *cd;
	/* destroy designated master first, to avoid EXTSYNC reference flapping */
	if(_masterClock != NULL) {
	    _masterClock->setState(_masterClock, CS_FREERUN, CSR_INTERNAL);
	    freeClockDriver(&_masterClock);
	}
	LL_DESTROYALL(cd, freeClockDriver);
	_systemClock = NULL;

}

bool
createClockDriversFromString(const char* list, bool (*pushConfig) (ClockDriver *, const void*), const void *config, bool quiet) {

	ClockDriverSpec spec;
	ClockDriver *cd = NULL;
	int namelen = 0;
	int pathlen = 0;
	memset(&spec, 0, sizeof(spec));

	foreach_token_begin(clockspecs, list, specLine, DEFAULT_TOKEN_DELIM);

	    if(!parseClockDriverSpec(specLine, &spec)) {
		counter_clockspecs--;
		continue;
	    }

	    pathlen = strlen(spec.path);
	    namelen = strlen(spec.name);

	    if( (namelen <= 0) && (pathlen > 0) ) {
		strncpy(spec.name, spec.path + ( pathlen <= CCK_CLOCKDRIVER_PATH_NAME_MAX ? 0 : pathlen - CCK_CLOCKDRIVER_PATH_NAME_MAX), CCK_COMPONENT_NAME_MAX);
	    }

	    namelen = strlen(spec.name);

	    if( (pathlen <= 0) && (namelen > 0) ) {
		strncpy(spec.path, spec.name, CCK_COMPONENT_NAME_MAX);
	    }

	    if(!strlen(spec.name) && !strlen(spec.path)) {
		CCK_ERROR(THIS_COMPONENT"Clock driver string: \"%s\": no name or path given\n", specLine);
		counter_clockspecs--;
		continue;
	    }

	    if((cd = getClockDriverByName(spec.name))) {
		if(!quiet) {
		    CCK_WARNING(THIS_COMPONENT"Clock driver string: \"%s\" : clock driver %s already exists\n", specLine, spec.name);
		}
	    } else if((cd = findClockDriver(spec.path))) {
		if(!quiet) {
		    CCK_WARNING(THIS_COMPONENT"Clock driver string: \"%s\" : cannot create another clock driver of this type\n", specLine);
		}
	    } else {
		cd = createClockDriver(spec.type, spec.name);
	    }

	    if(cd != NULL) {
		if(!cd->_init) {
		    CCK_INFO(THIS_COMPONENT"Extra clock starting: name %s type %s path %s\n", spec.name, getClockDriverTypeName(spec.type), spec.path);
		    cd->init(cd, spec.path);
		}
		cd->inUse = true;
		pushConfig(cd, config);
	    }

	foreach_token_end(clockspecs);

	return true;

}


static void
setState(ClockDriver *driver, ClockState newState, ClockStateReason reason) {

	if(driver == NULL) {
	    return;
	}

	if(newState >= CS_MAX) {
	    CCK_DBG(THIS_COMPONENT"Clock %s: setState(): unknown state %d\n", driver->name,
			newState);
	    return;
	}

	if((newState > CS_FREERUN) && driver->config.disabled) {
	    return;
	}

	/* todo: switch/case FSM leaving+entering as we get more conditions */
	if(driver->state != newState) {

	    driver->counters.stateChanges++;

	    if(newState < CS_MAX && reason < CSR_MAX) {
		driver->counters.stateStats[newState][reason]++;
	    }

	    CCK_NOTICE(THIS_COMPONENT"Clock %s changed state from %s to %s (reason: %s)\n",
		    driver->name, getClockStateName(driver->state),
		    getClockStateName(newState), getClockStateReasonDesc(reason));

	    getSystemClock()->getTimeMonotonic(getSystemClock(), &driver->_lastUpdate);
	    tsOps.clear(&driver->age);

	    /* if we're going into FREERUN, but not from a "good" state, restore last good frequency */
	    if( (newState == CS_FREERUN) && !(driver->state == CS_LOCKED || driver->state == CS_HOLDOVER)) {
		driver->restoreFrequency(driver);
	    }

	    if( (newState == CS_LOCKED) && !driver->wasLocked) {
		driver->wasLocked = true;
		driver->minAdev = driver->adev;
		driver->maxAdev = driver->adev;
	    }

	    /* entering or leaving locked state */
	    if((newState == CS_LOCKED) || (driver->state == CS_LOCKED)) {

		/* do not sync this clock until next findBestClock() */
		driver->_waitForElection = true;

		CCK_DBG(THIS_COMPONENT"Clock '%s' adev %.03f minAdev %.03f maxAdev %.03f minAdevTotal %.03f maxAdevTotal %.03f totalAdev %.03f\n",
		driver->name, driver->adev, driver->minAdev, driver->maxAdev, driver->minAdevTotal, driver->maxAdevTotal, driver->totalAdev);
	    }

	    if(newState == CS_HWFAULT) {
		driver->config.faultTimeout = getCckConfig()->clockFaultTimeout;
		driver->setReference(driver, NULL);
		SAFE_CALLBACK(driver->callbacks.onClockFault, driver, driver->owner, true);
	    }

	    if(driver->state == CS_HWFAULT) {
		SAFE_CALLBACK(driver->callbacks.onClockFault, driver, driver->owner, false);
	    }

	    driver->lastState = driver->state;
	    driver->lastStateReason = driver->stateReason;
	    driver->state = newState;
	    driver->stateReason = reason;

	    SAFE_CALLBACK(driver->callbacks.onStateChange, driver, driver->owner, driver->lastState, driver->state);
	    SAFE_CALLBACK(driver->callbacks.onLock, driver, driver->owner, newState == CS_LOCKED);

	}

}

void
updateClockDrivers(int interval) {

	ClockDriver *cd;
	CckTimestamp now;

	LL_FOREACH_STATIC(cd) {

	    if(cd->config.disabled) {
		continue;
	    }

	    if(cd->_warningTimeout > 0) {
		cd->_warningTimeout -= interval;
	    }

	    getSystemClock()->getTimeMonotonic(getSystemClock(), &now);
	    tsOps.sub(&cd->age, &now, &cd->_lastUpdate);

	    CCK_DBGV(THIS_COMPONENT"Clock %s age %d.%d\n", cd->name, cd->age.seconds, cd->age.nanoseconds);

	    switch(cd->state) {

		case CS_HWFAULT:
		    if(cd->age.seconds >= cd->config.faultTimeout) {
			if(cd->healthCheck(cd)) {
			    cd->setState(cd, CS_FREERUN, CSR_FAULT);
			} else {
			    cd->touchClock(cd);
			}
		    }
		    break;
		case CS_INIT:
		    break;
		case CS_STEP:
		    if(cd->age.seconds >= cd->config.stepTimeout) {
			CCK_WARNING(THIS_COMPONENT"Clock %s suspension delay timeout, resuming clock updates\n", cd->name);
			cd->setState(cd, CS_FREERUN, CSR_QUALITY);
			cd->_canResume = true;
		    }
		    break;
		case CS_NEGSTEP:
		    break;
		case CS_FREERUN:
		    if((cd->refClock == NULL) && (!cd->externalReference)) {
			if(_bestClock != NULL) {
			    cd->setReference(cd, _bestClock);
			}
		    }
		    break;
		case CS_FREQEST:
		    /* calibration timeout should only trigger on clock sync - see estimateFrequency() */
		    break;
		case CS_LOCKED:
			if((cd->refClock == NULL) && (!cd->externalReference)) {
			    cd->setState(cd, CS_HOLDOVER, CSR_REFCHANGE);
			    resetIntPermanentAdev(&cd->_adev);
			    break;
			} else if(cd->refClock != NULL) {
			    if((cd->refClock->state != CS_LOCKED) && (cd->refClock->state != CS_HOLDOVER)) {
				cd->setState(cd, CS_HOLDOVER, CSR_REFCHANGE);
				resetIntPermanentAdev(&cd->_adev);
				cd->setReference(cd, NULL);
				break;
			    }
			}
			if(!cd->maintainLock && (cd->age.seconds > cd->config.lockedAge)) {
			    resetIntPermanentAdev(&cd->_adev);
			    cd->setState(cd, CS_HOLDOVER, CSR_IDLE);
			}
		    break;
		case CS_TRACKING:
			if((cd->refClock == NULL) && (!cd->externalReference)) {
			    resetIntPermanentAdev(&cd->_adev);
			    cd->setState(cd, CS_FREERUN, CSR_REFCHANGE);
			    break;
			}
		    break;
		case CS_HOLDOVER:
			if(cd->age.seconds > cd->config.holdoverAge) {
			    cd->setState(cd, CS_FREERUN, CSR_AGE);
			}
		    break;
		default:
		    CCK_DBG(THIS_COMPONENT"Clock driver %s in unknown state %02d\n", cd->name, cd->state);

	    }

	    SAFE_CALLBACK(cd->callbacks.onUpdate, cd, cd->owner);

	}

	findBestClock();

}

void
syncClocks(double tau) {

    _syncInterval = tau;

    ClockDriver *cd;

    /* sync locked clocks first, in case if they are to unlock */
    LL_FOREACH_STATIC(cd) {

	    if(cd->config.disabled) {
		continue;
	    }

	    if(cd->state == CS_LOCKED) {
		cd->syncClock(cd, tau);
		/* this will prevent the clock from syncing again below - only for internal reference */
		if(!cd->externalReference) {
		    cd->_skipSync = 1;
		}
	    }
    }
    /* sync the whole rest */
    LL_FOREACH_STATIC(cd) {

	    if((cd->config.disabled) || (cd->state == CS_HWFAULT)) {
		continue;
	    }

	    cd->syncClock(cd, tau);

    }


}

void
stepClocks(bool force) {

    ClockDriver *cd;
    LL_FOREACH_STATIC(cd) {

	if((cd->config.disabled) || (cd->state == CS_HWFAULT)) {
	    continue;
	}

	if((cd != _bestClock) && ((cd->refClock != NULL) || cd->externalReference)) {
	CCK_DBG("%s STEP ref %d.%d\n", cd->name, cd->refOffset.seconds, cd->refOffset.nanoseconds);
	    cd->stepTime(cd, &cd->refOffset, force);
	}
    }

    /* step the best clock last so that other clocks don't lose its reference beforehand */
    if(_bestClock != NULL) {
	    _bestClock->stepTime(_bestClock, &_bestClock->refOffset, force);
    }

}

void
reconfigureClockDrivers(bool (*pushConfig)(ClockDriver*, const void*), const void *config) {

    ClockDriver *cd;

    LL_FOREACH_STATIC(cd) {
	pushConfig(cd, config);
    }

	/* system clock cannot be disabled */
	getSystemClock()->config.disabled = false;

    findBestClock();

}

static void
processUpdate(ClockDriver *driver) {

	bool update = false;

	if(driver->config.disabled) {
	    return;
	}

	if(driver->state == CS_HWFAULT) {
	    return;
	}

	feedIntPermanentAdev(&driver->_adev, driver->lastFrequency);
	driver->totalAdev = feedIntPermanentAdev(&driver->_totalAdev, driver->lastFrequency);

	if(driver->servo.runningMaxOutput) {
	    /* resetIntPermanentAdev(&driver->_adev); */
	}
	/* we have enough allan dev samples to represent adev period */
	if( (driver->_tau > ZEROF) && ((driver->_adev.count * driver->_tau * driver->servo.delayFactor) 
					> driver->config.adevPeriod) ) {
	    driver->adev = driver->_adev.adev;

	    if(driver->adev > ZEROF) {
		if(!driver->adevValid) {
		    driver->minAdevTotal = driver->adev;
		    driver->maxAdevTotal = driver->adev;
		    }

		if(driver->adev > driver->maxAdevTotal) {
		    driver->maxAdevTotal = driver->adev;
		}

		if(driver->adev < driver->minAdevTotal) {
		    driver->minAdevTotal = driver->adev;
		}
	    }

	    CCK_DBG(THIS_COMPONENT"clock %s  ADEV %.09f\n", driver->name, driver->adev);

	    if(driver->state == CS_LOCKED) {
		if(driver->adev > driver->maxAdev) {
		    driver->maxAdev = driver->adev;
		}
		if(driver->adev < driver->minAdev) {
		    driver->minAdev = driver->adev;
		}
	    }

	    if((driver->state == CS_STEP) || (driver->state == CS_NEGSTEP)) {
		update = false;
	    } else if (!driver->config.readOnly) {
		if(driver->servo.runningMaxOutput) {
		    driver->setState(driver, CS_TRACKING, CSR_QUALITY);
		} else if(driver->adev <= driver->config.stableAdev) {
		    driver->storeFrequency(driver);
		    driver->setState(driver, CS_LOCKED, CSR_QUALITY);
		} else if((driver->adev >= driver->config.unstableAdev) && (driver->state == CS_LOCKED)) {
		    driver->setState(driver, CS_TRACKING, CSR_QUALITY);
		}
		update = true;
	    }
	    driver->adevValid = true;
	    resetIntPermanentAdev(&driver->_adev);
	}

	if(driver->state == CS_FREERUN) {
	    if(!driver->config.readOnly) {
		driver->setState(driver, CS_TRACKING, CSR_SYNC);
	    }
	    update = true;
	}

	if((driver->state == CS_HOLDOVER) && (driver->age.seconds <= driver->config.holdoverAge)) {
	    driver->setState(driver, CS_TRACKING, CSR_SYNC);
	    update = true;
	}

	if((driver->state == CS_LOCKED) && driver->servo.runningMaxOutput) {
	    driver->setState(driver, CS_TRACKING, CSR_QUALITY);
	    update = true;
	}

	if((driver->state == CS_NEGSTEP) && !tsOps.isNegative(&driver->refOffset)) {
	    driver->lockedUp = false;
	    driver->setState(driver, CS_FREERUN, CSR_OFFSET);
	    update = true;
	}

	CCK_DBG(THIS_COMPONENT"clock %s  total ADEV %.09f\n", driver->name, driver->totalAdev);

	getSystemClock()->getTimeMonotonic(getSystemClock(), &driver->_lastSync);

	driver->touchClock(driver);

	if(update) {
	    updateClockDrivers(0);
	}

}

static void
setReference(ClockDriver *a, ClockDriver *b) {

    if(a == NULL) {
	return;
    }

    if(a->config.disabled) {
	return;
    }

    if( (b != NULL) && a->config.externalOnly) {
	CCK_DBG("Clock %s only accepts external reference clocks\n", a->name);
	return;
    }

    if(a == b) {
	CCK_ERROR(THIS_COMPONENT"Cannot set reference of clock %s to self\n", a->name);
	return;
    }

    /* loop detection - do not allow a reference to use
     * a reference which uses itself, directly or indirectly
     */
    if(b != NULL) {
	ClockDriver *cd;
	int hops = 0;
	for( cd = b->refClock; cd != NULL; cd = cd->refClock) {
	    hops++;
	    if(cd == a) {
		CCK_NOTICE(THIS_COMPONENT"Cannot set reference of clock %s to %s: %s already references %s (%d hops)\n",
		    a->name, b->name, a->name, b->name, hops);
		return;
	    }
	}
    }

    /* no change */
    if((a->refClock != NULL) && (a->refClock == b)) {
	return;
    }

    if(b == NULL && a->refClock != NULL) {
	CCK_NOTICE(THIS_COMPONENT"Clock %s lost reference %s\n", a->name, a->refClock->name);
	a->lastRefClass = a->refClass;
	a->refClock = NULL;
	memset(a->refName, 0, CCK_COMPONENT_NAME_MAX);
	if(a->state == CS_LOCKED) {
	    a->setState(a, CS_HOLDOVER, CSR_REFCHANGE);
	} else {
	    a->distance = 255;
	}
	a->refClass = RC_NONE;
	goto finalise;
	
    }

    if(b == NULL && a->externalReference) {
	CCK_NOTICE(THIS_COMPONENT"Clock %s lost external reference %s\n", a->name, a->refName);
	/* reset owner and owner callbacks */
	a->owner = NULL;
	memset(&a->callbacks, 0, sizeof(a->callbacks));
	a->externalReference = false;
	memset(a->refName, 0, CCK_COMPONENT_NAME_MAX);
	a->lastRefClass = a->refClass;
	a->refClock = NULL;
	if(a->state == CS_LOCKED) {
	    a->setState(a, CS_HOLDOVER, CSR_REFCHANGE);
	} else {
	    a->distance = 255;
	}
	a->refClass = RC_NONE;
	goto finalise;
    } else if (b != NULL) {
	CCK_NOTICE(THIS_COMPONENT"Clock %s changed reference from %s to %s\n", a->name, a->refClock == NULL ? "none" : a->refClock->name, b->name);
	if(a->refClock == NULL) {
	    a->lastRefClass = RC_NONE;
	}
	a->refClass = RC_INTERNAL;
	a->externalReference = false;
	a->refClock = b;
	a->distance = b->distance + 1;
	strncpy(a->refName, b->name, CCK_COMPONENT_NAME_MAX);

	a->setState(a, CS_FREERUN, CSR_REFCHANGE);

    }



// WOJ:CHECK

/*
    if(a->systemClock && (b!= NULL) && !b->systemClock) {
	a->servo.kI = b->servo.kI;
	a->servo.kP = b->servo.kP;
    }
*/

    return;

finalise:

    /* preferred master clock has lost reference - automatically assign imaginary external reference and lock it */
    if(a == _masterClock && b == NULL) {
	a->setExternalReference(a, getCckConfig()->masterClockRefName, RC_EXTERNAL);
	a->maintainLock = true;
	a->setState(a, CS_LOCKED, CSR_FORCED);
    }

}

static void setExternalReference(ClockDriver *a, const char* refName, int refClass) {

	if(a == NULL) {
	    return;
	}

	if(a->config.disabled) {
	    return;
	}

	if( a->config.internalOnly) {
	    CCK_DBG("Clock %s only accepts internal reference clocks\n", a->name);
	    return;
	}

	if(!a->externalReference || strncmp(a->refName, refName, CCK_COMPONENT_NAME_MAX)) {
	    CCK_NOTICE(THIS_COMPONENT"Clock %s changed to external reference %s\n", a->name, refName);
	    a->setState(a, CS_FREERUN, CSR_REFCHANGE);
	}

	strncpy(a->refName, refName, CCK_COMPONENT_NAME_MAX);
	if(a->refClock == NULL) {
	    a->lastRefClass = RC_NONE;
	} else {
	    a->lastRefClass = a->refClass;
	}
	a->externalReference = true;
	a->refClass = refClass;
	a->refClock = NULL;
	a->distance = 1;

}

static void
restoreFrequency (ClockDriver *driver) {

    double frequency = 0.0;
    char frequencyPath [PATH_MAX * 2 + 3];
    memset(frequencyPath, 0, PATH_MAX * 2 + 3);

    if(driver->config.disabled) {
	return;
    }

    /* try to retrieve from file */
    if(driver->config.storeToFile) {
	snprintf(frequencyPath, PATH_MAX * 2 + 2, "%s/%s", driver->config.frequencyDir, driver->config.frequencyFile);
	if(!doubleFromFile(frequencyPath, &frequency)) {
	    frequency = driver->storedFrequency;
	}
    /* otherwise use stored frequency from last lock */
    } else {
	frequency = driver->storedFrequency;
    }

    /* goddamn floats! */
    if(fabs(frequency) <= ZEROF) {
	/* last resort - use current frequency */
	frequency = driver->getFrequency(driver);
    }

    frequency = clamp(frequency, driver->maxFrequency);

    driver->servo.prime(&driver->servo, frequency);
    driver->storedFrequency = driver->servo.output;
    driver->setFrequency(driver, driver->storedFrequency, 1.0);

}

static void
storeFrequency (ClockDriver *driver) {

    char frequencyPath [PATH_MAX * 2 + 3];
    memset(frequencyPath, 0, PATH_MAX * 2 + 3);

    if(driver->config.disabled) {
	return;
    }

    if(driver->config.storeToFile) {
	snprintf(frequencyPath, PATH_MAX * 2 + 2, "%s/%s", driver->config.frequencyDir, driver->config.frequencyFile);
	doubleToFile(frequencyPath, driver->lastFrequency);
    }

    driver->storedFrequency = driver->lastFrequency;

}

static bool
adjustFrequency (ClockDriver *driver, double adj, double tau) {

	if(driver->config.disabled) {
	    return false;
	}

	bool ret = driver->setFrequency(driver, adj , tau);
	driver->processUpdate(driver);
	return ret;

}

static bool
stepTime (ClockDriver* driver, CckTimestamp *delta, bool force)
{

	CckTimestamp newTime;

	if((!driver->_init) || (driver->state == CS_HWFAULT)) {
	    return false;
	}

	if((driver->config.readOnly) || (driver->config.disabled)) {
		return true;
	}

	if(tsOps.isZero(delta)) {
	    return true;
	}

	/* do not step the clock by less than config.minStep nanoseconds (if minStep set) */
	if(!delta->seconds && driver->config.minStep && (abs(delta->nanoseconds) <= driver->config.minStep)) {
	    /* act as if the step was successful */
	    driver->_stepped = true;
	    return true;
	}

	if(force) {
	    driver->lockedUp = false;
	} else {
	    /* no frequency estimate, start the process */
	    if(driver->config.calibrationTime && !driver->_frequencyEstimated) {
		driver->setState(driver, CS_FREQEST, CSR_QUALITY);
		return false;
	    }
	}

	if(!force && !driver->config.negativeStep && tsOps.isNegative(delta)) {
		CCK_CRITICAL(THIS_COMPONENT"Cannot step clock %s  backwards\n", driver->name);
		CCK_CRITICAL(THIS_COMPONENT"Manual intervention required or SIGUSR1 to force %s clock step\n", driver->name);
		driver->lockedUp = true;
		driver->setState(driver, CS_NEGSTEP, CSR_OFFSET);
		return false;
	}

	driver->getTime(driver, &newTime);
	tsOps.add(&newTime, &newTime, delta);

	driver->_frequencyEstimated = false;

	if(newTime.seconds == 0) {
	    CCK_ERROR(THIS_COMPONENT"Cannot step clock %s to zero seconds\n", driver->name);
	    return false;
	}

	if(newTime.seconds < 0) {
	    CCK_ERROR(THIS_COMPONENT"Cannot step clock %s to a negative value %d\n", driver->name, newTime.seconds);
	    return false;
	}

	if(!driver->setOffset(driver, delta)) {
	    CCK_ERROR("Could not step clock %s!\n", driver->name);
	    return false;
	}

	/* notify the owner that time has changed */
	SAFE_CALLBACK(driver->callbacks.onStep, driver, driver->owner);

	/* reset filters */

	if(driver->_madFilter) {
	    resetDoubleMovingStatFilter(driver->_madFilter);
	}
	if(driver->_filter) {
	    resetDoubleMovingStatFilter(driver->_filter);
	}

	CCK_NOTICE(THIS_COMPONENT"Stepped clock %s by %s%d.%09d seconds\n", driver->name,
		    (delta->seconds <0 || delta->nanoseconds <0) ? "-":"", abs(delta->seconds), abs(delta->nanoseconds));

	driver->_stepped = true;

	if(force || (driver->state != CS_FREQEST)) {
		driver->setState(driver, CS_FREERUN, CSR_QUALITY);
	}

	return true;


}

static void
resetClockAge(ClockDriver *driver) {

	if(driver->config.disabled) {
	    return;
	}

	getSystemClock()->getTimeMonotonic(getSystemClock(), &driver->_lastUpdate);
	driver->age.seconds = 0;
	driver->age.nanoseconds = 0;

}

/* mark an update */
static void
touchClock(ClockDriver *driver) {

	if(driver->config.disabled) {
	    return;
	}

	CckTimestamp now;
	getSystemClock()->getTimeMonotonic(getSystemClock(), &now);
	tsOps.sub(&driver->age, &now, &driver->_lastUpdate);
	getSystemClock()->getTimeMonotonic(getSystemClock(), &driver->_lastUpdate);
	driver->_updated = true;

}

static void
setFrequencyEstimate (ClockDriver *driver) {

    double frequency = driver->getFrequency(driver);

    CCK_DBG(THIS_COMPONENT"Clock set estimate: %s last freq: %f, new freq %f\n", driver->name, frequency, frequency + driver->estimatedFrequency);

    frequency += driver->estimatedFrequency;

    frequency = clamp(frequency, driver->maxFrequency);

    if(fabs(frequency) <= ZEROF) {
	return;
    }

    CCK_DBG("Clock %s setting estimated frequency to %.03f\n", driver->name, frequency);

    driver->servo.reset(&driver->servo);
    driver->servo.prime(&driver->servo, frequency);
    driver->storedFrequency = driver->servo.output;
    driver->setFrequency(driver, driver->storedFrequency, 1.0);

}

static bool
estimateFrequency(ClockDriver *driver, double tau) {

	double dDelta;
	CckTimestamp delta;

	if(driver->config.disabled) {
	    return false;
	}

	/* we will only get here if config has been changed while we were in FREQEST */
	if(driver->config.calibrationTime == 0) {
	    driver->_lastDelta.seconds = 0;
	    driver->_lastDelta.nanoseconds = 0;
	    driver->_estimateCount = 0;
	    resetDoublePermanentMean(&driver->_calMean);
	    driver->stepTime(driver, &driver->refOffset, false);
	    driver->setState(driver, CS_TRACKING, CSR_OFFSET);
	    return true;
	}

	/* first run */
	if(driver->_calMean.count == 0 && driver->_estimateCount == 0) {
	    resetClockAge(driver);
	    driver->_estimateCount = 0;
	    driver->_lastDelta = driver->refOffset;
	    driver->_frequencyEstimated = false;
	}

	driver->_estimateCount++;

	/* update frequency estimate at least every second */
	if((driver->_estimateCount * tau) >= CLOCKDRIVER_FREQEST_INTERVAL) {

	    tsOps.sub(&delta, &driver->refOffset, &driver->_lastDelta);
	    dDelta = tsOps.toDouble(&delta) / (driver->_estimateCount * tau);
	    feedDoublePermanentMean(&driver->_calMean, dDelta );
	    CCK_DBG(THIS_COMPONENT"estimateFrequency('%s): samples %.0f delta %.09f mean %.09f\n", driver->name,
		driver->_calMean.count, dDelta, driver->_calMean.mean);
	    driver->estimatedFrequency = driver->_calMean.mean  * 1E9;
	    driver->_lastDelta = driver->refOffset;
	    driver->_estimateCount = 0;

	}

	/* frequency estimation time is up */
	if(driver->age.seconds >=
	    max(max(CLOCKDRIVER_FREQEST_MIN_TAU * tau, CLOCKDRIVER_FREQEST_INTERVAL), driver->config.calibrationTime)) {
		CCK_INFO(THIS_COMPONENT"Clock %s estimated frequency error %.03f ppb\n",
			driver->name, driver->estimatedFrequency);
		tsOps.clear(&driver->_lastDelta);
		driver->_estimateCount = 0;
		driver->_frequencyEstimated = true;
		/* nothing to do if too little data for a mean */
		if(driver->_calMean.count > 1) {
		    setFrequencyEstimate(driver);
		}
		resetDoublePermanentMean(&driver->_calMean);
		driver->stepTime(driver, &driver->refOffset, false);
		driver->setState(driver, CS_TRACKING, CSR_OFFSET);
	}

	return true;

}

static bool
filterClock(ClockDriver *driver, double tau) {

	double dOffset = tsOps.toDouble(&driver->refOffset);

	/* stage 1: run the outlier filter */
	if(driver->config.outlierFilter) {

	    /* filter allows us to continue (enough samples to compute). */
	     if( feedDoubleMovingStatFilter(driver->_madFilter, dOffset)
		    /* filter delay condition met - enough samples */
		    && (driver->_madFilter->meanContainer->count >= driver->config.madDelay)) {

		double dev = fabs(dOffset - driver->_madFilter->output);
		double madd = dev / driver->_madFilter->output;

		if(madd > driver->config.madMax) {
#ifdef PTPD_CLOCK_SYNC_PROFILING
		    CCK_INFO(THIS_COMPONENT"prof Clock %s +outlier Offset %.09f mads %.09f MAD %.09f blocking for %.02f\n",
					    driver->name, dOffset, madd, driver->_madFilter->output, driver->_madFilter->blockingTime);
#else
		    CCK_DBG(THIS_COMPONENT"prof Clock %s +outlier Offset %.09f mads %.09f MAD %.09f blocking for %.02f\n",
					    driver->name, dOffset, madd, driver->_madFilter->output, driver->_madFilter->blockingTime);
#endif
		    driver->refOffset = driver->_lastOffset;

		    if(driver->_madFilter->lastBlocked) {
			driver->_madFilter->consecutiveBlocked++;
			driver->_madFilter->blockingTime += tau;
		    }

		    driver->_madFilter->lastBlocked = true;

		    /* we have been blocknig for too long */
		    if(driver->_madFilter->blockingTime > driver->config.outlierFilterBlockTimeout) {
			CCK_DBG(THIS_COMPONENT"Clock %s outlier filter blocking for more than %d seconds - resetting filter\n",
			driver->name, driver->config.outlierFilterBlockTimeout);
			resetDoubleMovingStatFilter(driver->_madFilter);
		    }
		    /* sample rejected */
		    return false;

		/* we continue */
		} else {
#ifdef PTPD_CLOCK_SYNC_PROFILING
		    CCK_INFO(THIS_COMPONENT"prof Clock %s: outlier Offset %.09f mads %.09f MAD %.09f\n", driver->name, dOffset, madd, driver->_madFilter->output);
#else
		    CCK_DBG(THIS_COMPONENT"prof Clock %s: outlier Offset %.09f mads %.09f MAD %.09f\n", driver->name, dOffset, madd, driver->_madFilter->output);
#endif
		    driver->_madFilter->lastBlocked = false;
		    driver->_madFilter->consecutiveBlocked = 0;
		    driver->_madFilter->blockingTime = 0;
		}
	    }
	}

	/* stage 2: statistical filter */
	if(driver->config.statFilter) {
	    if(!feedDoubleMovingStatFilter(driver->_filter, dOffset)) {
		driver->refOffset = driver->_lastOffset;
		if(driver->_filter->lastBlocked) {
		    driver->_filter->consecutiveBlocked++;
		}
		driver->_filter->lastBlocked = true;
		return false;
	    } else {
		    driver->_filter->lastBlocked = false;
		    driver->_filter->consecutiveBlocked = 0;
	    }
	    driver->refOffset = tsOps.fromDouble(driver->_filter->output);
	}

	return true;

}

static bool
disciplineClock(ClockDriver *driver, CckTimestamp offset, double tau) {

    char buf[100];
    double dOffset;

    CckTimestamp offsetCorrection = { 0, driver->config.offsetCorrection };

    if(driver->config.disabled) {
	return false;
    }

    if(driver->_skipSync > 0) {
	driver->_skipSync--;
	return false;
    }

    driver->_lastOffset = driver->refOffset;
    driver->rawOffset = offset;
    driver->refOffset = offset;

    tsOps.sub(&driver->refOffset, &driver->refOffset, &offsetCorrection);

    /* run filter if running internal reference, drop sample if filter discards it */
    if((!driver->externalReference || driver->config.alwaysFilter) && !filterClock(driver, tau)) {
	driver->rawOffset = driver->_lastOffset;
	driver->refOffset = driver->_lastOffset;
	return false;
    }

    /* do nothing if offset is zero - prevents from linked clocks being dragged around,
     * and it's not obvious that two clocks can be linked, as we can see with Intel,
     * where multiple NIC ports can present themselves as different clock IDs, but
     * in fact all show the same frequency (or so it seems...)
     */

    if(tsOps.isZero(&driver->refOffset) || driver->config.readOnly) {
	driver->lastFrequency = driver->getFrequency(driver);
	driver->processUpdate(driver);
	return true;
    }

    dOffset = tsOps.toDouble(&driver->refOffset);

    if(!driver->config.readOnly) {

	/* forced step on first update */
	if((driver->config.stepType == CSTEP_STARTUP_FORCE) && !driver->_updated && !driver->_stepped && !driver->lockedUp) {
		return driver->stepTime(driver, &offset, false);
	}

	if(driver->refOffset.seconds) {

		memset(buf, 0, sizeof(buf));
		snprint_CckTimestamp(buf, sizeof(buf), &driver->refOffset);

		int sign = (driver->refOffset.seconds < 0) ? -1 : 1;

		/* step on first update */
		if((driver->config.stepType == CSTEP_STARTUP) && !driver->_updated && !driver->_stepped && !driver->lockedUp) {
			return driver->stepTime(driver, &offset, true);
		}

		/* panic mode */
		if(driver->state == CS_STEP) {
			return false;
		}

		/* we refused to step backwards and offset is still negative */
		if((sign == -1) && driver->state == CS_NEGSTEP) {
			return false;
		}

		/* going into panic mode */
		if(driver->config.stepTimeout) {
			/* ...or in fact exiting it... */
			if(driver->_canResume) {
/* WOJ:CHECK */
/* we're assuming that we can actually step! */
			} else {
			    CCK_WARNING(THIS_COMPONENT"Clock %s offset above 1 second (%s s), suspending clock control for %d seconds (panic mode)\n",
				    driver->name, buf, driver->config.stepTimeout);
			    driver->setState(driver, CS_STEP, CSR_OFFSET);
			    return false;
			}
		}

		/* we're not allowed to step the clock */
		if(driver->config.noStep) {
			if(!driver->_warningTimeout) {
			    driver->_warningTimeout = CLOCKDRIVER_CCK_WARNING_TIMEOUT;
			    CCK_WARNING(THIS_COMPONENT"Clock %s offset above 1 second (%s s), clock step disabled, slewing at max rate (%.0f us/s)\n",
				driver->name, buf, (sign * driver->servo.maxOutput) / 1000.0);
			}
			driver->servo.prime(&driver->servo, sign * driver->servo.maxOutput);
			driver->_canResume = false;
			return driver->adjustFrequency(driver, sign * driver->servo.maxOutput, tau);
		}

		if(driver->state != CS_FREQEST) {
		    CCK_WARNING(THIS_COMPONENT"Clock %s offset above 1 second (%s s), attempting to step the clock\n", driver->name, buf);
		    if( driver->stepTime(driver, &offset, false) ) {
			driver->_canResume = false;
			tsOps.clear(&driver->refOffset);
			return true;
		    } else {
			return false;
		    }
		} else {
		    return estimateFrequency(driver, tau);
		}

	} else {

		if(driver->state == CS_STEP) {
		    /* we are outside the exit threshold, clock is still suspended */
		    if(driver->config.stepExitThreshold && (labs(driver->refOffset.nanoseconds) > driver->config.stepExitThreshold)) {
			return false;
		    }
		    CCK_NOTICE(THIS_COMPONENT"Clock %s offset below 1 second, resuming clock control\n", driver->name);
		    driver->setState(driver, CS_FREERUN, CSR_OFFSET);
		}

		if(driver->state == CS_NEGSTEP) {
		    driver->lockedUp = false;
		    driver->setState(driver, CS_FREERUN, CSR_OFFSET);
		}

		if(driver->state == CS_FREQEST) {
			return estimateFrequency(driver, tau);
		}

		if(driver->config.stepDetection && driver->state == CS_LOCKED) {
		    /* simulate next servo run before feeding it, calculate next frequency delta to last frequency */
		    double fwdDelta = driver->servo.tau *
					fabs(driver->servo.simulate(&driver->servo, dOffset) -
						driver->servo._lastOutput);

		    CCK_DBG("disciplineClock('%s'): nextdelta %.03f tau %.03f\n", driver->name, fwdDelta, driver->servo.tau);

		    /* check if a frequency jump would occur */
		    if(fwdDelta > driver->config.unstableAdev) {
			CCK_NOTICE(THIS_COMPONENT"disciplineClock('%s'): discarded %d ns offset to prevent a frequency jump\n",
					driver->name, driver->refOffset.nanoseconds, fwdDelta, driver->config.unstableAdev);

			SAFE_CALLBACK(driver->callbacks.onFrequencyJump, driver, driver->owner);

			driver->setState(driver, CS_HOLDOVER, CSR_QUALITY);

			driver->rawOffset = driver->_lastOffset;
			driver->refOffset = driver->_lastOffset;

			return false;
		    }

		}

		/* offset accepted, feed it to servo and adjust clock frequency */
		driver->servo.feed(&driver->servo, dOffset, tau);
		return driver->adjustFrequency(driver, driver->servo.output, tau);

	}

    }

    return false;

}

static bool
syncClock(ClockDriver* driver, double tau) {

	if(driver->config.disabled) {
	    return false;
	}

	CckTimestamp delta;

	/* nothing to sync with */
	if(driver->externalReference || (driver->refClock == NULL)) {
	    /* update frequency information */
	    driver->lastFrequency = driver->getFrequency(driver);
	    return false;
	}

	/* do not sync if we are awaiting best clock change */
	if(driver->_waitForElection || driver->refClock->_waitForElection) {
	    CCK_DBG(THIS_COMPONENT"Will not sync clock %s until next best clock election\n",
		driver->name);
	    return false;
	}

	/* explicitly prevent sync from "bad" clocks if strict sync enabled */
	if(driver->config.strictSync && (driver->refClock->state < CS_HOLDOVER)) {
	    CCK_DBG(THIS_COMPONENT"Will not sync clock %s with reference (%s) in %s state\n",
		driver->name, driver->refClock->name, getClockStateName(driver->refClock->state));
	    return false;
	}

	if(!driver->getOffsetFrom(driver, driver->refClock, &delta)) {
	    return false;
	}

	return disciplineClock(driver, delta, tau);

}

static bool
syncClockExternal(ClockDriver* driver, CckTimestamp offset, double tau) {

    if(driver->config.disabled) {
	return false;
    }

    if(!driver->externalReference) {
	return false;
    }

    /* do not sync if we are awaiting best clock change */
    if(driver->_waitForElection) {
	CCK_DBG(THIS_COMPONENT"Will not sync clock %s until next best clock election\n",
	    driver->name);
	return false;
    }

    return disciplineClock(driver, offset, tau);

}

const char*
getClockDriverTypeName(int type) {

    if ((type < 0) || (type >= CLOCKDRIVER_MAX)) {
	return NULL;
    }

    return clockDriverNames[type];

}

int
getClockDriverType(const char* name) {


    #define CCK_REGISTER_IMPL(typeenum, typesuffix, textname) \
	if(!strcmp(name, textname)) {\
	    return typeenum;\
	}\

    #include "clockdriver.def"

    return -1;

}

bool
parseClockDriverSpec(const char* line, ClockDriverSpec *spec) {

	memset(spec, 0, sizeof(ClockDriverSpec));
	spec->type = -1;

	foreach_token_begin(params, line, param, ":");
		switch(counter_params) {
		    case 0:
			spec->type = getClockDriverType(param);
			if(spec->type == -1) {
			    CCK_ERROR(THIS_COMPONENT"Clock driver string \"%s\" : unknown clock driver type: %s\n", line, param);
			}
			break;
		    case 1:
			strncpy(spec->path, param, PATH_MAX);
			break;
		    case 2:
			strncpy(spec->name, param, CCK_COMPONENT_NAME_MAX);
			break;
		    default:
			break;
		}
	foreach_token_end(params);

	if(counter_params < 1) {
	    CCK_ERROR(THIS_COMPONENT"Clock driver string: \"%s\": no parameters given\n", line);
	    return false;
	}

	if(spec->type == -1) {
	    return false;
	}

	return true;

}

const char*
getClockStateName(ClockState state) {

    switch(state) {
	case CS_SUSPENDED:
	    return "SUSPENDED";
	case CS_HWFAULT:
	    return "HWFAULT";
	case CS_NEGSTEP:
	    return "NEGSTEP";
	case CS_STEP:
	    return "STEP";
	case CS_INIT:
	    return "INIT";
	case CS_FREERUN:
	    return "FREERUN";
	case CS_FREQEST:
	    return "FREQEST";
	case CS_LOCKED:
	    return "LOCKED";
	case CS_TRACKING:
	    return "TRACKING";
	case CS_HOLDOVER:
	    return "HOLDOVER";
	default:
	    return "UNKNOWN";
    }

}

const char*
getClockStateShortName(ClockState state) {

    switch(state) {
	case CS_SUSPENDED:
	    return "SUSP";
	case CS_HWFAULT:
	    return "HWFL";
	case CS_NEGSTEP:
	    return "NSTP";
	case CS_STEP:
	    return "STEP";
	case CS_INIT:
	    return "INIT";
	case CS_FREQEST:
	    return "FEST";
	case CS_FREERUN:
	    return "FREE";
	case CS_LOCKED:
	    return "LOCK";
	case CS_TRACKING:
	    return "TRCK";
	case CS_HOLDOVER:
	    return "HOLD";
	default:
	    return "UNKN";
    }

}

const char*
getClockStateReasonName(ClockStateReason reason) {

    switch(reason) {
	case CSR_INTERNAL:	/* internal FSM work: init, shutdown, etc */
	    return "INTERNAL";
	case CSR_OFFSET:	/* state change because of reference offset value */
	    return "OFFSET";
	case CSR_IDLE:		/* clock was idle */
	    return "IDLE";
	case CSR_SYNC:		/* resumed sync */
	    return "SYNC";
	case CSR_AGE:		/* state age: holdover->freerun, etc. */
	    return "TIMEOUT";
	case CSR_REFCHANGE:	/* reference */
	    return "REFCHANGE";
	case CSR_QUALITY:	/* offset change / re-sync */
	    return "QUALITY";
	case CSR_FORCED:
	    return "FORCED";
	case CSR_FAULT:		/* fault */
	    return "FAULT";
	case CSR_OTHER:
	default:
	    return "OTHER";
    }

}

const char*
getClockStateReasonDesc(ClockStateReason reason) {

    switch(reason) {
	case CSR_INTERNAL:	/* internal FSM work: init, shutdown, etc */
	    return "init / shutdown";
	case CSR_OFFSET:	/* state change because of reference offset value */
	    return "offset change";
	case CSR_IDLE:		/* clock was idle */
	    return "idle / no updates";
	case CSR_SYNC:		/* resumed sync */
	    return "synchronising";
	case CSR_AGE:		/* state age: holdover->freerun, etc. */
	    return "timeout";
	case CSR_REFCHANGE:	/* reference */
	    return "reference change";
	case CSR_QUALITY:	/* offset change / re-sync */
	    return "quality change";
	case CSR_FORCED:
	    return "forced";
	case CSR_FAULT:		/* fault */
	    return "fault";
	case CSR_OTHER:
	default:
	    return "unknown";
    }

}


ClockDriver*
findClockDriver(const char * search) {

	ClockDriver *cd;
	LL_FOREACH_STATIC(cd) {
	    if(cd->isThisMe(cd, search)) {
		return cd;
	    }
	}

	return NULL;

}

ClockDriver*
getClockDriverByName(const char * search) {

	ClockDriver *cd;

	LL_FOREACH_STATIC(cd) {
	    if(!strncmp(cd->name, search, CCK_COMPONENT_NAME_MAX)) {
		return cd;
	    }
	}

	return NULL;

}

void
compareAllClocks() {

    int count = 0;
    int lineLen = 20 * 50 + 1;

    char lineBuf[lineLen];
    char timeBuf[100];
    CckTimestamp delta;

    ClockDriver *outer;
    ClockDriver *inner;
    ClockDriver *cd;

    memset(lineBuf, 0, lineLen);
    memset(timeBuf, 0, sizeof(timeBuf));

    LL_FOREACH_STATIC(cd) {
	count += snprintf(lineBuf + count, lineLen - count, "\t\t%s", cd->name);
    }

    CCK_INFO("%s\n", lineBuf);

    memset(lineBuf, 0, lineLen);
    count = 0;

    LL_FOREACH_STATIC(outer) {

	memset(lineBuf, 0, lineLen);
	count = 0;
	count += snprintf(lineBuf + count, lineLen - count, "%s\t", outer->name);

	LL_FOREACH_STATIC(inner) {
	    memset(timeBuf, 0, sizeof(timeBuf));
	    outer->getOffsetFrom(outer, inner, &delta);
	    snprint_CckTimestamp(timeBuf, sizeof(timeBuf), &delta);
	    count += snprintf(lineBuf + count, lineLen - count, "%s\t", timeBuf);
	}

	CCK_INFO("%s\n", lineBuf);

    }

}

void configureClockDriverFilters(ClockDriver *driver) {

    StatFilterOptions opts, madOpts;
    ClockDriverConfig *config = &driver->config;

    memset(&opts, 0, sizeof(StatFilterOptions));
    memset(&madOpts, 0, sizeof(StatFilterOptions));

    /* filters are reset on reconfig... */
    freeDoubleMovingStatFilter(&driver->_filter);
    freeDoubleMovingStatFilter(&driver->_madFilter);

    opts.enabled = config->statFilter;
    opts.filterType = config->filterType;
    opts.windowSize = config->filterWindowSize;
    opts.windowType = config->filterWindowType;

    madOpts.enabled = config->outlierFilter;
    madOpts.filterType = FILTER_MAD;
    madOpts.windowSize = config->madWindowSize;
    madOpts.windowType = WINDOW_SLIDING;

    if(config->madDelay > config->madWindowSize) {
	config->madDelay = config->madWindowSize;
    }

    driver->_filter = createDoubleMovingStatFilter(&opts, driver->name);
    driver->_madFilter = createDoubleMovingStatFilter(&madOpts, driver->name);

}

void
controlClockDrivers(int command, const void *arg) {

    ClockDriver *cd;
    bool found;

    switch(command) {

	case CD_NOTINUSE:
	    LL_FOREACH_STATIC(cd) {
		if(!cd->systemClock) {
		    cd->inUse = false;
		    cd->config.required = false;
		    /* clean up the owner and reset callbacks */
		    cd->owner = NULL;
		    memset(&cd->callbacks, 0, sizeof(cd->callbacks));
		}
	    }
	break;

	case CD_SHUTDOWN:
	    LL_DESTROYALL(cd, freeClockDriver);
	    _systemClock = NULL;
	break;

	case CD_CLEANUP:
	    do {
		found = false;
		LL_FOREACH_STATIC(cd) {
		    /* the system clock should always be kept */
		    if(!cd->inUse && !cd->systemClock) {
			freeClockDriver(&cd);
			found = true;
			break;
		    }
		}
	    } while(found);
	break;

	case CD_DUMP:
	    LL_FOREACH_STATIC(cd) {
		if(cd->config.disabled) {
		    continue;
		}
		CCK_INFO(THIS_COMPONENT"Clock driver %s state %s\n", cd->name, getClockStateName(cd->state));
		/* todo: dump clock info with counters */
	    }
	break;

	case CD_SKIPSYNC:
	    LL_FOREACH_STATIC(cd) {
		if(cd->config.disabled || !cd->inUse) {
		    continue;
		}
		cd->_skipSync = *(const int*)arg;
	    }
	break;


	default:
	    CCK_ERROR(THIS_COMPONENT"Unnown clock driver command %02d\n", command);

    }

}

ClockDriver*
compareClockDriver(ClockDriver *a, ClockDriver *b) {

	/* if one is null, the other wins, if both are null, no winner */
	if((a != NULL) && (b == NULL) ) {
	    return a;
	}

	if((a == NULL) && (b != NULL) ) {
	    return b;
	}

	if((a == NULL) && (b == NULL)) {
	    return NULL;
	}

	/* if one is disabled, the other wins, if both are, no winner */
	if((!a->config.disabled) && (b->config.disabled) ) {
	    return a;
	}

	if((!b->config.disabled) && (a->config.disabled) ) {
	    return b;
	}

	if((a->config.disabled) && (b->config.disabled) ) {
	    return NULL;
	}

	/* if one is excluded, the other wins, if both are, no winner */
	if((!a->config.excluded) && (b->config.excluded) ) {
	    return a;
	}

	if((!b->config.excluded) && (a->config.excluded) ) {
	    return b;
	}

	if((a->config.excluded) && (b->config.excluded) ) {
	    return NULL;
	}

	/* better state than us - wins */
	if( (a->state < b->state) && (b->state > CS_FREERUN)) {
		/* however, do not prefer a locked with higher distance
		 * to a holdover with lower distance */
		if((a->state == CS_HOLDOVER) && (b->state == CS_LOCKED)) {
		    if(a->distance < b->distance) {
			return a;
		    }
		}
		    return b;
	}

	/* our state is better - we win */
	if(b->state < a->state && (a->state > CS_FREERUN) ) {
		/* however, do not prefer a locked with higher distance
		 * to a holdover with lower distance */
		if((b->state == CS_HOLDOVER) && (a->state == CS_LOCKED)) {
		    if(b->distance < a->distance) {
			return b;
		    }
		}
		return a;
	}

	/* same state */
	if(a->state == b->state) {

		if(a->state == CS_LOCKED || a->state == CS_HOLDOVER) {

		    /* external reference is better */
		    if(a->externalReference && !b->externalReference) {
			return a;
		    }

		    if(!a->externalReference && b->externalReference) {
			return b;
		    }

		    /* lower reference class is better */
		    if(a->externalReference && b->externalReference) {
			if(a->refClass > b-> refClass) {
			    return a;
			}
			if(b->refClass > a-> refClass) {
			    return b;
			}
		    }

		    /* clock referencing best clock is better than one with no reference or referencing non-best */
		    if((a->refClock && (a->refClock==_bestClock)) &&
			(!b->refClock || (b->refClock != _bestClock))) {
			return a;
		    }

		    if((b->refClock && (b->refClock==_bestClock)) &&
			(!a->refClock || (a->refClock != _bestClock))) {
			return b;
		    }

		    /* clock referencing the system clock is worse */
		    if(a->refClock && b->refClock) {
			if(!a->refClock->systemClock && b->refClock->systemClock) {
			    return a;
			}
			if(a->refClock->systemClock && !b->refClock->systemClock) {
			    return b;
			}
		    }

		    /* ref class of last used reference if both in holdover */
		    if(a->state == CS_HOLDOVER) {
			if(a->lastRefClass > b->lastRefClass) {
			    return a;
			}
			if(b->lastRefClass > a->lastRefClass) {
			    return b;
			}
		    }

		    /* tiebreaker 1: lower reference chain hop count wins */
		    if(a->distance < b->distance) {
			return a;
		    }

		    if(a->distance > b->distance) {
			return b;
		    }


		    /* tiebreaker 2: system clock loses */
		    if(!a->systemClock && b->systemClock) {
			return a;
		    }
		    if(a->systemClock && !b->systemClock) {
			return b;
		    }

		    /* tiebreaker 3: lower allan dev wins */
		    if((a->adev > ZEROF) && (b->adev > ZEROF)) {

			if(a->adev < b->adev) {
			    return a;
			}

			if(a->adev > b->adev) {
			    return b;
			}
		    }

		    /* final tiebreaker - clock longer in state wins */
		    if(a->age.seconds > b->age.seconds) {
			return a;
		    }

		    if(a->age.seconds < b->age.seconds) {
			return b;
		    }

		}

	}
	/* erm... what would John Eidson do? */
	return a;
}

/* dummy placeholder callback */
int
clockDriverDummyCallback(ClockDriver* self) {
    return 1;
}

static void
findBestClock() {

    ClockDriver *newBest = NULL;
    ClockDriver *cd;

    LL_FOREACH_STATIC(cd) {
	if((cd->config.disabled) || (cd->config.excluded)) {
	    continue;
	}
	if(cd->state == CS_LOCKED) {
	    newBest = cd;
	    break;
	}
    }

    if(newBest == NULL) {
	LL_FOREACH_STATIC(cd) {
	    if((cd->config.disabled) || (cd->config.excluded)) {
		continue;
	    }
	    if(cd->state == CS_HOLDOVER) {
		newBest = cd;
		break;
	    }
	}
    }

    if(newBest != NULL) {
	LL_FOREACH_STATIC(cd) {
	    if((cd->config.disabled) || (cd->state == CS_HWFAULT) || (cd->config.excluded)) {
		continue;
	    }
	    if(newBest != cd) {
		    newBest = compareClockDriver(newBest, cd);
	    }
    }

	if(newBest != _bestClock ) {
	    CCK_NOTICE(THIS_COMPONENT"New best clock selected: %s\n", newBest->name);
	}

    } else {

	if(newBest != _bestClock ) {
	    CCK_NOTICE(THIS_COMPONENT"No best clock available\n");
	}

    }

    if(newBest != _bestClock) {
	if(_bestClock != NULL) {
	    _bestClock->bestClock = false;
	    /* we are changing best reference - drop the old one */
	    LL_FOREACH_STATIC(cd) {
		if(cd->config.disabled) {
		    continue;
		}
		if(!cd->externalReference && (cd->refClock == _bestClock)
		    && (cd->state != CS_LOCKED) && (cd->state != CS_HOLDOVER)) {
		    cd->setReference(cd, NULL);
		}
	    }
	}
	_bestClock = newBest;
	if(_bestClock != NULL) {
	    _bestClock->bestClock = true;
	}

	LL_FOREACH_STATIC(cd) {
	    if(cd->config.disabled) {
		continue;
	    }
	    if(!cd->externalReference && (cd != _bestClock)) {
		cd->setReference(cd, NULL);
	    }
	}

	LL_FOREACH_STATIC(cd) {
	    if(cd->config.disabled) {
		continue;
	    }
	    if(!cd->externalReference && (cd != _bestClock)) {
		cd->setReference(cd, _bestClock);
	    }
	}
    }

    /* mark ready for sync */
    LL_FOREACH_STATIC(cd) {
	    cd->_waitForElection = false;
    }

}

static void
putStatsLine(ClockDriver* driver, char* buf, int len) {

    char tmpBuf[100];
    memset(tmpBuf, 0, sizeof(tmpBuf));
    memset(buf, 0, len);
    snprint_CckTimestamp(tmpBuf, sizeof(tmpBuf), &driver->refOffset);

    if(driver->config.disabled) {
	snprintf(buf, len - 1, "disabled");
    } else {
	snprintf(buf, len - 1, "%s%s offs: %-13s  adev: %-8.3f freq: %.03f", driver->config.readOnly ? "r" : " ",
	    driver->bestClock ? "*" : driver->state <= CS_INIT ? "!" : driver->config.excluded ? "-" : " ",
	    tmpBuf, driver->adev, driver->lastFrequency);
    }
}

static void
putInfoLine(ClockDriver* driver, char* buf, int len) {

    char tmpBuf[100];
    char tmpBuf2[30];
    memset(tmpBuf, 0, sizeof(tmpBuf2));
    memset(tmpBuf, 0, sizeof(tmpBuf));
    memset(buf, 0, len);
    snprint_CckTimestamp(tmpBuf, sizeof(tmpBuf), &driver->refOffset);

    if((driver->state == CS_STEP) && driver->config.stepTimeout) {
	snprintf(tmpBuf2, sizeof(tmpBuf2), "%s %-4ld", getClockStateShortName(driver->state), driver->config.stepTimeout - driver->age.seconds);
    } else if((driver->state == CS_HWFAULT) && driver->config.faultTimeout) {
	snprintf(tmpBuf2, sizeof(tmpBuf2), "%s %-4ld", getClockStateShortName(driver->state), driver->config.faultTimeout - driver->age.seconds);
    } else {
	strncpy(tmpBuf2, getClockStateName(driver->state), CCK_COMPONENT_NAME_MAX);
    }

    if(driver->config.disabled) {
	snprintf(buf, len - 1, "disabled");
    } else {
	snprintf(buf, len - 1, "%s%s name:  %-12s state: %-9s ref: %-7s", driver->config.readOnly ? "r" : " ",
		driver->bestClock ? "*" : driver->state <= CS_INIT ? "!" : driver->config.excluded ? "-" : " ",
	    driver->name, tmpBuf2, strlen(driver->refName) ? driver->refName : "none");
    }
}

static bool healthCheck(ClockDriver *driver) {

    bool ret = true;

    if(driver == NULL) {
	return false;
    }

    CCK_DBG(THIS_COMPONENT"clock %s health check...\n", driver->name);

    ret &= driver->privateHealthCheck(driver);
    ret &= driver->_vendorHealthCheck(driver);

    return ret;
}

int parseLeapFile(ClockLeapInfo *info, const char *path)
{
    FILE *leapFP;
    CckTimestamp now;
    char lineBuf[PATH_MAX];

    unsigned long ntpSeconds = 0;
    int32_t utcSeconds = 0;
    int32_t utcExpiry = 0;
    int ntpOffset = 0;
    int res;

    getSystemClock()->getTime(getSystemClock(), &now);

    info->valid = false;

    if( (leapFP = fopen(path,"r")) == NULL) {
	CCK_PERROR("Could not open leap second list file %s", path);
	return 0;
    } else

    memset(info, 0, sizeof(ClockLeapInfo));

    while (fgets(lineBuf, PATH_MAX - 1, leapFP) != NULL) { 

	/* capture file expiry time */
	res = sscanf(lineBuf, "#@ %lu", &ntpSeconds);
	if(res == 1) {
	    utcExpiry = ntpSeconds - NTP_EPOCH;
	    CCK_DBG(THIS_COMPONENT"parseLeapInfo('%s'): leapfile expiry %d\n", path, utcExpiry);
	}
	/* capture leap seconds information */
	res = sscanf(lineBuf, "%lu %d", &ntpSeconds, &ntpOffset);
	if(res ==2) {
	    utcSeconds = ntpSeconds - NTP_EPOCH;
	    CCK_DBG(THIS_COMPONENT"parseLeapInfo('%s'): date %d offset %d\n", path, utcSeconds, ntpOffset);

	    /* next leap second date found */

	    if((now.seconds ) < utcSeconds) {
		info->nextOffset = ntpOffset;
		info->endTime = utcSeconds;
		info->startTime = utcSeconds - 86400;
		break;
	    } else
	    /* current leap second value found */
		if(now.seconds >= utcSeconds) {
		info->currentOffset = ntpOffset;
	    }

	}

    }

    fclose(leapFP);

    /* leap file past expiry date */
    if(utcExpiry && utcExpiry < now.seconds) {
	CCK_WARNING(THIS_COMPONENT"parseLeapInfo('%s'): Leap seconds file is expired. Please download the current version.\n",
		    path);
	return 0;
    }

    /* we have the current offset - the rest can be invalid but at least we have this */
    if(info->currentOffset != 0) {
	info->offsetValid = true;
    }

    /* if anything failed, return 0 so we know we cannot use leap file information */
    if((info->startTime == 0) || (info->endTime == 0) ||
	(info->currentOffset == 0) || (info->nextOffset == 0)) {
	return 0;
	CCK_INFO(THIS_COMPONENT"parseLeapInfo('%s') Leap seconds file loaded (incomplete): now %d, current %d next %d from %d to %d, type %s\n", path,
	now.seconds,
	info->currentOffset, info->nextOffset,
	info->startTime, info->endTime, info->leapType > 0 ? "positive" : info->leapType < 0 ? "negative" : "unknown");
    }

    if(info->nextOffset > info->currentOffset) {
	info->leapType = 1;
    }

    if(info->nextOffset < info->currentOffset) {
	info->leapType = -1;
    }

    CCK_INFO(THIS_COMPONENT"parseLeapInfo('%s'): Leap seconds file loaded: now %d, current %d next %d from %d to %d, type %s\n", path,
	now.seconds,
	info->currentOffset, info->nextOffset,
	info->startTime, info->endTime, info->leapType > 0 ? "positive" : info->leapType < 0 ? "negative" : "unknown");
    info->valid = true;
    return 1;

}

void
setCckMasterClock(ClockDriver *newMaster)
{

    /* need to preserve last master before assigning */
    ClockDriver* lastMaster = _masterClock;

    /* we assign the master clock early so that last master is not master when it loses reference */
    _masterClock = newMaster;

    /* we have an existing master clock and it is losing its master status */
    if( (lastMaster != NULL) && (newMaster != lastMaster)) {

	/* if master clock loses reference, it is automatically locked to imaginary external reference, this is why we assigned earlier */
	lastMaster->setReference(lastMaster, NULL);
	lastMaster->maintainLock = false;
	lastMaster->setState(lastMaster, CS_FREERUN, CSR_REFCHANGE);

    }

    if((_masterClock != NULL) && (_masterClock->refClass == RC_NONE)) {

	_masterClock->setExternalReference(_masterClock, getCckConfig()->masterClockRefName, RC_EXTERNAL);
	_masterClock->maintainLock = true;
	_masterClock->setState(_masterClock, CS_LOCKED, CSR_FORCED);

    }

}

ClockDriver*
getCckMasterClock()
{

    return _masterClock;

}
