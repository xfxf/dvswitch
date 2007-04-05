// Copyright 2007 Ben Hutchings and Tore Sinding Bekkedal.
// See the file "COPYING" for licence details.

#include <iostream>
#include <memory>
#include <ostream>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "dv_display_widget.hpp"
#include "frame.h"

// X headers come last due to egregious macro pollution.
#include "gtk_x_utils.hpp"
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xvlib.h>

namespace
{
    const int frame_max_width = 720;
    const int frame_max_height = 576;

    // Assume 4:3 frame ratio for now.
    const int display_width_full = 768;
    const int display_height_full = 576;
    const int display_width_thumb = display_width_full / 4;
    const int display_height_thumb = display_height_full / 4;

    const uint32_t invalid_xv_port = uint32_t(-1);

    char * allocate_x_shm(Display * display, XShmSegmentInfo * info,
			  std::size_t size)
    {
	char * result = 0;
	if ((info->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0777)) != -1)
	{
	    info->shmaddr = static_cast<char *>(shmat(info->shmid, 0, 0));
	    if (info->shmaddr != reinterpret_cast<char *>(-1)
		&& XShmAttach(display, info))
		result = info->shmaddr;
	    shmctl(info->shmid, IPC_RMID, 0);
	}
	return result;
    }

    void free_x_shm(XShmSegmentInfo * info)
    {
	shmdt(info->shmaddr);
    }
}

dv_display_widget::dv_display_widget(int quality)
    : decoder_(dv_decoder_new(0, true, true)),
      decoded_serial_num_(-1)
{
    set_app_paintable(true);
    set_double_buffered(false);
    dv_set_quality(decoder_, quality);
}

dv_display_widget::~dv_display_widget()
{
    dv_decoder_free(decoder_);
}

dv_full_display_widget::dv_full_display_widget()
    : dv_display_widget(DV_QUALITY_BEST),
      xv_port_(invalid_xv_port),
      xv_image_(0),
      xv_shm_info_(0)
{
    set_size_request(display_width_full, display_height_full);
}

dv_thumb_display_widget::dv_thumb_display_widget()
    : dv_display_widget(DV_QUALITY_FASTEST),
      x_image_(0),
      x_shm_info_(0)
{
    set_size_request(display_width_thumb, display_height_thumb);
}

void dv_full_display_widget::on_realize()
{
    dv_display_widget::on_realize();

    assert(xv_port_ == invalid_xv_port && xv_image_ == 0);

    Display * display = get_x_display(*this);

    unsigned adaptor_count;
    XvAdaptorInfo * adaptor_info;

    if (XvQueryAdaptors(display, get_x_window(*this),
			&adaptor_count, &adaptor_info) != Success)
    {
	std::cerr << "ERROR: XvQueryAdaptors() failed\n";
	return;
    }

    // Search for a suitable adaptor.
    unsigned i;
    for (i = 0; i != adaptor_count; ++i)
    {
	if (!(adaptor_info[i].type & XvImageMask))
	    continue;
	int format_count;
	XvImageFormatValues * format_info =
	    XvListImageFormats(display, adaptor_info[i].base_id,
			       &format_count);
	if (!format_info)
	    continue;
	for (int j = 0; j != format_count; ++j)
	    if (format_info[j].id == pixel_format_id)
		goto end_adaptor_loop;
    }
end_adaptor_loop:
    if (i == adaptor_count)
    {
	std::cerr << "ERROR: No Xv adaptor for this display supports "
		  << char(pixel_format_id >> 24)
		  << char((pixel_format_id >> 16) & 0xFF)
		  << char((pixel_format_id >> 8) & 0xFF)
		  << char(pixel_format_id & 0xFF)
		  << " format\n";
    }
    else
    {
	// Try to allocate a port.
	unsigned j;
	for (j = 0; j != adaptor_info[i].num_ports; ++j)
	{
	    XvPortID port = adaptor_info[i].base_id + i;
	    if (XvGrabPort(display, port, CurrentTime) == Success)
	    {
		xv_port_ = port;
		break;
	    }
	}
	if (j == adaptor_info[i].num_ports)
	    std::cerr << "ERROR: Could not grab an Xv port\n";
    }

    XvFreeAdaptorInfo(adaptor_info);

    if (xv_port_ == invalid_xv_port)
	return;

    if (XShmSegmentInfo * xv_shm_info = new (std::nothrow) XShmSegmentInfo)
    {
	if (XvImage * xv_image =
	    XvShmCreateImage(display, xv_port_, pixel_format_id, 0,
			     frame_max_width, frame_max_height,
			     xv_shm_info))
	{
	    if ((xv_image->data = allocate_x_shm(display, xv_shm_info,
						 xv_image->data_size)))
	    {
		xv_image_ = xv_image;
		xv_shm_info_ = xv_shm_info;
	    }
	    else
	    {
		free(xv_image);
		delete xv_shm_info;
	    }
	}
	else
	{
	    delete xv_shm_info;
	}
    }
}

void dv_full_display_widget::on_unrealize()
{
    if (xv_port_ != invalid_xv_port)
    {
	Display * display = get_x_display(*this);

	XvStopVideo(display, xv_port_, get_x_window(*this));

	if (XvImage * xv_image = static_cast<XvImage *>(xv_image_))
	{
	    xv_image_ = 0;
	    free(xv_image);
	    XShmSegmentInfo * xv_shm_info =
		static_cast<XShmSegmentInfo *>(xv_shm_info_);
	    xv_shm_info_ = 0;
	    free_x_shm(xv_shm_info);
	    delete xv_shm_info;
	}

	XvUngrabPort(display, xv_port_, CurrentTime);
	xv_port_ = invalid_xv_port;
    }
}

void dv_display_widget::put_frame(const mixer::frame_ptr & dv_frame)
{
    if (dv_frame->serial_num != decoded_serial_num_)
    {
	pixels_pitch buffer = get_frame_buffer();
	if (!buffer.first)
	    return;

	dv_parse_header(decoder_, dv_frame->buffer);
	dv_decode_full_frame(decoder_, dv_frame->buffer,
			     e_dv_color_yuv, &buffer.first, &buffer.second);
	decoded_serial_num_ = dv_frame->serial_num;

	draw_frame(decoder_->width, decoder_->height);
    }
}

dv_display_widget::pixels_pitch dv_full_display_widget::get_frame_buffer()
{
    if (XvImage * xv_image = static_cast<XvImage *>(xv_image_))
    {
	assert(xv_image->num_planes == 1);
	return pixels_pitch(reinterpret_cast<uint8_t *>(xv_image->data),
			    xv_image->pitches[0]);
    }
    else
    {
	return pixels_pitch(0, 0);
    }
}

void dv_full_display_widget::draw_frame(unsigned width, unsigned height)
{
    XvImage * xv_image = static_cast<XvImage *>(xv_image_);
    // XXX should use get_window()->get_internal_paint_info()
    Display * display = get_x_display(*this);
    if (Glib::RefPtr<Gdk::GC> gc = Gdk::GC::create(get_window()))
    {
	XvShmPutImage(display, xv_port_,
		      get_x_window(*this), gdk_x11_gc_get_xgc(gc->gobj()),
		      xv_image,
		      0, 0, width, height,
		      0, 0, display_width_full, display_height_full,
		      False);
	XFlush(display);
    }
}

void dv_thumb_display_widget::on_realize()
{
    dv_display_widget::on_realize();

    Display * display = get_x_display(*this);
    int screen = 0; // XXX should use gdk_x11_screen_get_screen_number
    XVisualInfo visual_info;
    if (XMatchVisualInfo(display, screen, 24, DirectColor, &visual_info))
    {
	if (XShmSegmentInfo * x_shm_info = new (std::nothrow) XShmSegmentInfo)
	{
	    // TODO: We actually need to create an RGB buffer at the
	    // display size and a YUY2 buffer at the video frame
	    // size.  But this will do for a rough demo.
	    if (XImage * x_image = XShmCreateImage(
		    display, visual_info.visual, 24, ZPixmap,
		    0, x_shm_info, frame_max_width, frame_max_height))
	    {
		if ((x_image->data = allocate_x_shm(
			 display, x_shm_info,
			 x_image->height * x_image->bytes_per_line)))
		{
		    x_image_ = x_image;
		    x_shm_info_ = x_shm_info;
		}
		else
		{
		    free(x_image);
		}
	    }
	    if (!x_shm_info_)
		delete x_shm_info;
	}
    }
}

void dv_thumb_display_widget::on_unrealize()
{
    if (XImage * x_image = static_cast<XImage *>(x_image_))
    {
	XShmSegmentInfo * x_shm_info =
	    static_cast<XShmSegmentInfo *>(x_shm_info_);
	free_x_shm(x_shm_info);
	delete x_shm_info;
	x_shm_info_ = 0;
	free(x_image);
	x_image = 0;
    }

    dv_display_widget::on_unrealize();
}

dv_display_widget::pixels_pitch dv_thumb_display_widget::get_frame_buffer()
{
    if (XImage * x_image = static_cast<XImage *>(x_image_))
    {
	// XXX This needs to return the YUY2 buffer.
	return pixels_pitch(reinterpret_cast<uint8_t *>(x_image->data),
			    x_image->bytes_per_line);
    }
    else
    {
	return pixels_pitch(0, 0);
    }
}

void dv_thumb_display_widget::draw_frame(unsigned width, unsigned height)
{
    // XXX This needs to convert and scale between the two buffers.
    XImage * x_image = static_cast<XImage *>(x_image_);
    // XXX should use get_window()->get_internal_paint_info()
    Display * display = get_x_display(*this);
    if (Glib::RefPtr<Gdk::GC> gc = Gdk::GC::create(get_window()))
    {
	XShmPutImage(display,
		     get_x_window(*this), gdk_x11_gc_get_xgc(gc->gobj()),
		     x_image,
		     0, 0,
		     0, 0, display_width_thumb, display_height_thumb,
		     False);
	XFlush(display);
    }
}
