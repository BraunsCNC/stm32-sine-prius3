#ifndef ANAIN_PRJ_H_INCLUDED
#define ANAIN_PRJ_H_INCLUDED

#include "hwdefs.h"

#define NUM_SAMPLES 12
#define SAMPLE_TIME ADC_SMPR_SMP_7DOT5CYC

#define ANA_IN_LIST \
   ANA_IN_ENTRY(throttle1, GPIOC, 1) \
   ANA_IN_ENTRY(throttle2, GPIOC, 0) \
   ANA_IN_ENTRY(udc,       GPIOC, 3) \
   ANA_IN_ENTRY(tmpm,      GPIOC, 2) \
   ANA_IN_ENTRY(tmphs,     GPIOC, 4) \
   ANA_IN_ENTRY(uaux,      GPIOA, 3) \
   ANA_IN_ENTRY(il1,       GPIOA, 5) \
   ANA_IN_ENTRY(il2,       GPIOB, 0)

#define ANA_IN_LIST_BLUEPILL \
   ANA_IN_ENTRY(throttle1, GPIOA, 0) \
   ANA_IN_ENTRY(throttle2, GPIOA, 1) \
   ANA_IN_ENTRY(udc,       GPIOA, 2) \
   ANA_IN_ENTRY(tmpm,      GPIOA, 3) \
   ANA_IN_ENTRY(tmphs,     GPIOA, 4) \
   ANA_IN_ENTRY(uaux,      GPIOB, 1) \
   ANA_IN_ENTRY(il1,       GPIOA, 5) \
   ANA_IN_ENTRY(il2,       GPIOB, 0)

#endif // ANAIN_PRJ_H_INCLUDED
