#include "dsee_compat.h"
#include <string.h>
int main(void) {
    return strcmp(dsee_version(), "0.1.0") || dsee_destroy(NULL) != DSEE_OK;
}
