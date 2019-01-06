#include "profiler.h"
#include "stm32f4xx_hal.h"
#include "string.h"
#include <stdbool.h>
#include "functions.h"

PROFILE_INFO profiles[PROFILES_COUNT];

void InitProfiler()
{
	for(uint8_t i=0;i<PROFILES_COUNT;i++)
	{
		profiles[i].startTime=0;
		profiles[i].endTime=0;
		profiles[i].diff=0;
		profiles[i].samples=0;
		profiles[i].started=false;
	}
}

void StartProfiler(uint8_t pid)
{
	if(pid>=PROFILES_COUNT) return;
	if(profiles[pid].started) return;
	profiles[pid].started=true;
	profiles[pid].startTime=HAL_GetTick();
}

void StartProfilerTick()
{
	if(profiles[0].started) return;
	profiles[0].started=true;
	profiles[0].startTime=0;
	SCB_DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT_CYCCNT = 0;
	DWT_CONTROL |= DWT_CTRL_CYCCNTENA_Msk;
}

void EndProfiler(uint8_t pid)
{
	if(pid>=PROFILES_COUNT) return;
	if(!profiles[pid].started) return;
	profiles[pid].endTime=HAL_GetTick();
	profiles[pid].diff=profiles[pid].endTime-profiles[pid].startTime;
	profiles[pid].samples++;
	profiles[pid].started=false;
}

void EndProfilerTick()
{
	if(!profiles[0].started) return;
	profiles[0].endTime=DWT_CYCCNT/(SystemCoreClock / 1000000);
	DWT_CONTROL &= ~DWT_CTRL_CYCCNTENA_Msk;
	profiles[0].diff=+profiles[0].endTime;
	profiles[0].samples++;
	profiles[0].started=false;
}

void PrintProfilerResult()
{
	bool printed=false;
	for(uint8_t i=0;i<PROFILES_COUNT;i++)
		if(profiles[i].samples>0)
			{
				logToUART1_str("Profile #");
				logToUART1_numinline(i);
				logToUART1_str(": ");
				logToUART1_num32(profiles[i].diff);
				profiles[i].diff=0;
				profiles[i].samples=0;
				printed=true;
			}
	if(printed)
		logToUART1_str("\r\n");
}