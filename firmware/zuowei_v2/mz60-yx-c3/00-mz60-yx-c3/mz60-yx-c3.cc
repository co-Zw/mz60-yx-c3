#include "config.h"
#include "../firmware/zuowei_v2/mz60-yx-c3/00-mz60-yx-c3/mz60-yx-c3.h"

#define TAG "ZwMz01C3Lcd"

class ZwMz01C3Lcd : public Mz01C3Lcd {
public:
    ZwMz01C3Lcd(){

    }
    ~ZwMz01C3Lcd(){

    }
};

DECLARE_BOARD(ZwMz01C3Lcd);
