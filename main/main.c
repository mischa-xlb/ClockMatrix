#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "max7219.h"

// ---------------------------------------------------------------------------
// Display test:
//  - Shows each digit 0-9 cycling across all four panels.
//  - Panels are labelled 0 (leftmost) to 3 (rightmost).
// ---------------------------------------------------------------------------
void app_main(void)
{
    max7219_init();

    // Initial static display: show 1, 2, 3, 4
    max7219_set_digit(0, 1);
    max7219_set_digit(1, 2);
    max7219_set_digit(2, 3);
    max7219_set_digit(3, 4);

    vTaskDelay(pdMS_TO_TICKS(2000));

    // Cycle all digits 0-9 on every panel in turn
    while (1) {
        for (uint8_t d = 0; d <= 9; d++) {
            for (uint8_t m = 0; m < 4; m++) {
                max7219_set_digit(m, d);
            }
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
}
