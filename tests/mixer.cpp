#include <cstdlib>
#include <iostream>
#include <ostream>

#include <libdv/dv.h>

#include "frame.h"
#include "mixer.hpp"

// The use of volatile in this test program is not an endorsement of its
// use in production multithreaded code.  It probably works here, but I
// wouldn't want to depend on it.

namespace
{
    class dummy_sink : public mixer::sink
    {
    public:
	dummy_sink(volatile unsigned & sink_count)
	    : sink_count_(sink_count)
	{}
    private:
	virtual void put_frame(const mixer::frame_ptr &)
	{
	    std::cout << "sinked frame\n";
	    ++sink_count_;
	}
	virtual void cut()
	{
	    std::cout << "sinked cut\n";
	}
	volatile unsigned & sink_count_;
    };
}

int main()
{
    volatile unsigned sink_count = 0;
    unsigned source_count = 0;
    mixer the_mixer;
    the_mixer.add_source();
    the_mixer.add_sink(new dummy_sink(sink_count));
    for (;;)
    {
	if (source_count - sink_count < 8)
	{
	    mixer::frame_ptr frame(the_mixer.allocate_frame());
	    frame->system = e_dv_system_625_50;
	    the_mixer.put_frame(0, frame);
	    ++source_count;
	    std::cout << "sourced frame\n";
	    if ((std::rand() & 0x1F) == 0)
	    {
		the_mixer.cut();
		std::cout << "cut\n";
	    }
	}
	usleep(10000);
    }
}