/*****************************************************************************
 * vout_subpictures.c : subpicture management functions
 *****************************************************************************
 * Copyright (C) 2000 VideoLAN
 * $Id: vout_subpictures.c,v 1.8 2002/02/19 00:50:20 sam Exp $
 *
 * Authors: Vincent Seguin <seguin@via.ecp.fr>
 *          Samuel Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <stdio.h>                                              /* sprintf() */
#include <string.h>                                            /* strerror() */

#include <videolan/vlc.h>

#include "video.h"
#include "video_output.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void vout_RenderSPU( const vout_thread_t *p_vout, picture_t *p_pic,
                            const subpicture_t *p_spu );

/*****************************************************************************
 * vout_DisplaySubPicture: display a subpicture unit
 *****************************************************************************
 * Remove the reservation flag of a subpicture, which will cause it to be
 * ready for display.
 *****************************************************************************/
void  vout_DisplaySubPicture( vout_thread_t *p_vout, subpicture_t *p_subpic )
{
    int         i_margin;

    /* Check if status is valid */
    if( p_subpic->i_status != RESERVED_SUBPICTURE )
    {
        intf_ErrMsg( "vout error: subpicture %p has invalid status #%d",
                     p_subpic, p_subpic->i_status );
    }

    /* If the user requested an SPU margin, we force the position after
     * having checked that it was a valid value. */
    i_margin = main_GetIntVariable( VOUT_SPUMARGIN_VAR,
                                    VOUT_SPUMARGIN_DEFAULT );

    if( i_margin >= 0 )
    {
        if( p_subpic->i_height + i_margin <= p_vout->output.i_height )
        {
            p_subpic->i_y = p_vout->output.i_height
                             - i_margin - p_subpic->i_height;
        }
    }

    /* Remove reservation flag */
    p_subpic->i_status = READY_SUBPICTURE;
}

/*****************************************************************************
 * vout_CreateSubPicture: allocate a subpicture in the video output heap.
 *****************************************************************************
 * This function create a reserved subpicture in the video output heap.
 * A null pointer is returned if the function fails. This method provides an
 * already allocated zone of memory in the spu data fields. It needs locking
 * since several pictures can be created by several producers threads.
 *****************************************************************************/
subpicture_t *vout_CreateSubPicture( vout_thread_t *p_vout, int i_type,
                                     int i_size )
{
    int                 i_subpic;                        /* subpicture index */
    subpicture_t *      p_free_subpic = NULL;       /* first free subpicture */
    subpicture_t *      p_destroyed_subpic = NULL; /* first destroyed subpic */

    /* Get lock */
    vlc_mutex_lock( &p_vout->subpicture_lock );

    /*
     * Look for an empty place
     */
    for( i_subpic = 0; i_subpic < VOUT_MAX_SUBPICTURES; i_subpic++ )
    {
        if( p_vout->p_subpicture[i_subpic].i_status == DESTROYED_SUBPICTURE )
        {
            /* Subpicture is marked for destruction, but is still allocated */
            if( (p_vout->p_subpicture[i_subpic].i_type  == i_type)   &&
                (p_vout->p_subpicture[i_subpic].i_size  >= i_size) )
            {
                /* Memory size do match or is smaller : memory will not be
                 * reallocated, and function can end immediately - this is
                 * the best possible case, since no memory allocation needs
                 * to be done */
                p_vout->p_subpicture[i_subpic].i_status = RESERVED_SUBPICTURE;
                vlc_mutex_unlock( &p_vout->subpicture_lock );
                return( &p_vout->p_subpicture[i_subpic] );
            }
            else if( p_destroyed_subpic == NULL )
            {
                /* Memory size do not match, but subpicture index will be kept
                 * in case we find no other place */
                p_destroyed_subpic = &p_vout->p_subpicture[i_subpic];
            }
        }
        else if( (p_free_subpic == NULL) &&
                 (p_vout->p_subpicture[i_subpic].i_status == FREE_SUBPICTURE ))
        {
            /* Subpicture is empty and ready for allocation */
            p_free_subpic = &p_vout->p_subpicture[i_subpic];
        }
    }

    /* If no free subpictures are available, use a destroyed subpicture */
    if( (p_free_subpic == NULL) && (p_destroyed_subpic != NULL ) )
    {
        /* No free subpicture or matching destroyed subpictures have been
         * found, but a destroyed subpicture is still avalaible */
        free( p_destroyed_subpic->p_data );
        p_free_subpic = p_destroyed_subpic;
    }

    /* If no free or destroyed subpicture could be found */
    if( p_free_subpic == NULL )
    {
        intf_ErrMsg( "vout error: subpicture heap is full" );
        vlc_mutex_unlock( &p_vout->subpicture_lock );
        return( NULL );
    }

    /* Prepare subpicture */
    switch( i_type )
    {
    case TEXT_SUBPICTURE:                             /* text subpicture */
        p_free_subpic->p_data = memalign( 16, i_size + 1 );
        break;
    case DVD_SUBPICTURE:                          /* DVD subpicture unit */
        p_free_subpic->p_data = memalign( 16, i_size );
        break;
    default:
        intf_ErrMsg( "vout error: unknown subpicture type %d", i_type );
        p_free_subpic->p_data   =  NULL;
        break;
    }

    if( p_free_subpic->p_data != NULL )
    {
        /* Copy subpicture information, set some default values */
        p_free_subpic->i_type   = i_type;
        p_free_subpic->i_status = RESERVED_SUBPICTURE;
        p_free_subpic->i_size   = i_size;
        p_free_subpic->i_x      = 0;
        p_free_subpic->i_y      = 0;
        p_free_subpic->i_width  = 0;
        p_free_subpic->i_height = 0;
    }
    else
    {
        /* Memory allocation failed : set subpicture as empty */
        p_free_subpic->i_type   = EMPTY_SUBPICTURE;
        p_free_subpic->i_status = FREE_SUBPICTURE;
        p_free_subpic           = NULL;
        intf_ErrMsg( "vout error: spu allocation returned %s",
                     strerror( ENOMEM ) );
    }

    vlc_mutex_unlock( &p_vout->subpicture_lock );

    return( p_free_subpic );
}

/*****************************************************************************
 * vout_DestroySubPicture: remove a subpicture from the heap
 *****************************************************************************
 * This function frees a previously reserved subpicture.
 * It is meant to be used when the construction of a picture aborted.
 * This function does not need locking since reserved subpictures are ignored
 * by the output thread.
 *****************************************************************************/
void vout_DestroySubPicture( vout_thread_t *p_vout, subpicture_t *p_subpic )
{
   /* Check if status is valid */
   if( ( p_subpic->i_status != RESERVED_SUBPICTURE )
          && ( p_subpic->i_status != READY_SUBPICTURE ) )
   {
       intf_ErrMsg( "vout error: subpicture %p has invalid status %d",
                   p_subpic, p_subpic->i_status );
   }

    p_subpic->i_status = DESTROYED_SUBPICTURE;
}

/*****************************************************************************
 * vout_RenderSubPictures: render a subpicture list
 *****************************************************************************
 * This function renders a sub picture unit.
 *****************************************************************************/
void vout_RenderSubPictures( vout_thread_t *p_vout, picture_t *p_pic,
                             subpicture_t *p_subpic )
{
#if 0
    p_vout_font_t       p_font;                                 /* text font */
    int                 i_width, i_height;          /* subpicture dimensions */
#endif

    while( p_subpic != NULL )
    {
        switch( p_subpic->i_type )
        {
        case DVD_SUBPICTURE:                          /* DVD subpicture unit */
            vout_RenderSPU( p_vout, p_pic, p_subpic );
            break;

#if 0
        case TEXT_SUBPICTURE:                            /* single line text */
            /* Select default font if not specified */
            p_font = p_subpic->type.text.p_font;
            if( p_font == NULL )
            {
                p_font = p_vout->p_default_font;
            }

            /* Compute text size (width and height fields are ignored)
             * and print it */
            vout_TextSize( p_font, p_subpic->type.text.i_style,
                           p_subpic->p_data, &i_width, &i_height );
            if( !Align( p_vout, &p_subpic->i_x, &p_subpic->i_y,
                        i_width, i_height, p_subpic->i_horizontal_align,
                        p_subpic->i_vertical_align ) )
            {
                vout_Print( p_font,
                            p_vout->p_buffer[ p_vout->i_buffer_index ].p_data +
                            p_subpic->i_x * p_vout->i_bytes_per_pixel +
                            p_subpic->i_y * p_vout->i_bytes_per_line,
                            p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line,                            p_subpic->type.text.i_char_color,
                            p_subpic->type.text.i_border_color,
                            p_subpic->type.text.i_bg_color,
                            p_subpic->type.text.i_style, p_subpic->p_data, 100 );
                SetBufferArea( p_vout, p_subpic->i_x, p_subpic->i_y,
                               i_width, i_height );
            }
            break;
#endif

        default:
            intf_ErrMsg( "vout error: unknown subpicture %p type %d",
                         p_subpic, p_subpic->i_type );
            break;
        }

        p_subpic = p_subpic->p_next;
    }
}

/*****************************************************************************
 * vout_SortSubPictures: find the subpictures to display
 *****************************************************************************
 * This function parses all subpictures and decides which ones need to be
 * displayed. This operation does not need lock, since only READY_SUBPICTURE
 * are handled. If no picture has been selected, display_date will depend on
 * the subpicture.
 * We also check for ephemer DVD subpictures (subpictures that have
 * to be removed if a newer one is available), which makes it a lot
 * more difficult to guess if a subpicture has to be rendered or not.
 *****************************************************************************/
subpicture_t *vout_SortSubPictures( vout_thread_t *p_vout,
                                    mtime_t display_date )
{
    int i_index;
    subpicture_t *p_subpic     = NULL;
    subpicture_t *p_ephemer    = NULL;
    mtime_t       ephemer_date = 0;

    /* We get an easily parsable chained list of subpictures which
     * ends with NULL since p_subpic was initialized to NULL. */
    for( i_index = 0; i_index < VOUT_MAX_SUBPICTURES; i_index++ )
    {
        if( p_vout->p_subpicture[i_index].i_status == READY_SUBPICTURE )
        {
            /* If it is a DVD subpicture, check its date */
            if( p_vout->p_subpicture[i_index].i_type == DVD_SUBPICTURE )
            {
                if( display_date > p_vout->p_subpicture[i_index].i_stop )
                {
                    /* Too late, destroy the subpic */
                    vout_DestroySubPicture( p_vout,
                                    &p_vout->p_subpicture[i_index] );
                    continue;
                }

                if( display_date < p_vout->p_subpicture[i_index].i_start )
                {
                    /* Too early, come back next monday */
                    continue;
                }

                /* If this is an ephemer subpic, see if it's the
                 * youngest we have */
                if( p_vout->p_subpicture[i_index].b_ephemer )
                {
                    if( p_ephemer == NULL )
                    {
                        p_ephemer = &p_vout->p_subpicture[i_index];
                        continue;
                    }

                    if( p_vout->p_subpicture[i_index].i_start
                                                     < p_ephemer->i_start )
                    {
                        /* Link the previous ephemer subpicture and
                         * replace it with the current one */
                        p_ephemer->p_next = p_subpic;
                        p_subpic = p_ephemer;
                        p_ephemer = &p_vout->p_subpicture[i_index];

                        /* If it's the 2nd youngest subpicture,
                         * register its date */
                        if( !ephemer_date
                              || ephemer_date > p_subpic->i_start )
                        {
                            ephemer_date = p_subpic->i_start;
                        }

                        continue;
                    }
                }

                p_vout->p_subpicture[i_index].p_next = p_subpic;
                p_subpic = &p_vout->p_subpicture[i_index];

                /* If it's the 2nd youngest subpicture, register its date */                    if( !ephemer_date || ephemer_date > p_subpic->i_start )
                {
                    ephemer_date = p_subpic->i_start;
                }
            }
            /* If it's not a DVD subpicture, just register it */
            else
            {
                p_vout->p_subpicture[i_index].p_next = p_subpic;
                p_subpic = &p_vout->p_subpicture[i_index];
            }
        }
    }

    /* If we found an ephemer subpicture, check if it has to be
     * displayed */
    if( p_ephemer != NULL )
    {
        if( p_ephemer->i_start < ephemer_date )
        {
            /* Ephemer subpicture has lived too long */
            vout_DestroySubPicture( p_vout, p_ephemer );
        }
        else
        {
            /* Ephemer subpicture can still live a bit */
            p_ephemer->p_next = p_subpic;
            return p_ephemer;
        }
    }

    return p_subpic;
}

/*****************************************************************************
 * vout_RenderSPU: draw an SPU on a picture
 *****************************************************************************
 * This is a fast implementation of the subpicture drawing code. The data
 * has been preprocessed once in spu_decoder.c, so we don't need to parse the
 * RLE buffer again and again. Most sanity checks are done in spu_decoder.c
 * so that this routine can be as fast as possible.
 *****************************************************************************/
static void vout_RenderSPU( const vout_thread_t *p_vout, picture_t *p_pic,
                            const subpicture_t *p_spu )
{
    /* Common variables */
    u8   p_clut8[4], p_trsp[4];
    u16  p_clut16[4];
    u8  *p_dest;
    u16 *p_source = (u16 *)p_spu->p_data;

    int i_x, i_y;
    int i_len, i_color;

    /* RGB-specific */
    int i_xscale, i_yscale, i_width, i_height, i_ytmp, i_yreal, i_ynext;

    /* FIXME: get this from the DVD */
    p_trsp[0] = 0x00; p_trsp[1] = 0xff; p_trsp[2] = 0xff; p_trsp[3] = 0xff;

    switch( p_vout->output.i_chroma )
    {
    /* I420 target, no scaling */
    case FOURCC_I420:
    case FOURCC_IYUV:
    case FOURCC_YV12:

    /* FIXME: get this from the DVD */
    p_clut8[0] = 0xaa; p_clut8[1] = 0x44;
    p_clut8[2] = 0xff; p_clut8[3] = 0x88;

    p_dest = p_pic->p->p_pixels + p_spu->i_x + p_spu->i_width
              + p_vout->output.i_width * ( p_spu->i_y + p_spu->i_height );

    /* Draw until we reach the bottom of the subtitle */
    for( i_y = p_spu->i_height * p_vout->output.i_width ;
         i_y ;
         i_y -= p_vout->output.i_width )
    {
        /* Draw until we reach the end of the line */
        for( i_x = p_spu->i_width ; i_x ; )
        {
            /* Get the RLE part, then draw the line */
            i_color = *p_source & 0x3;

            switch( p_trsp[ i_color ] )
            {
                case 0x00:
                    i_x -= *p_source++ >> 2;
                    break;

                case 0xff:
                    i_len = *p_source++ >> 2;
                    memset( p_dest - i_x - i_y, p_clut8[ i_color ], i_len );
                    i_x -= i_len;
                    break;

                default:
                    /* FIXME: we should do transparency */
                    i_len = *p_source++ >> 2;
                    memset( p_dest - i_x - i_y, p_clut8[ i_color ], i_len );
                    i_x -= i_len;
                    break;
            }
        }
    }

    break;

    /* RV16 target, scaling */
    case FOURCC_RV16:

    /* FIXME: get this from the DVD */
    p_clut16[0] = 0xaaaa; p_clut16[1] = 0x4444;
    p_clut16[2] = 0xffff; p_clut16[3] = 0x8888;

    i_xscale = ( p_vout->output.i_width << 6 ) / p_vout->render.i_width;
    i_yscale = ( p_vout->output.i_height << 6 ) / p_vout->render.i_height;

    i_width  = p_spu->i_width  * i_xscale;
    i_height = p_spu->i_height * i_yscale;

    p_dest = p_pic->p->p_pixels + ( i_width >> 6 ) * 2
              /* Add the picture coordinates and the SPU coordinates */
              + ( (p_spu->i_x * i_xscale) >> 6 ) * 2
              + ( (p_spu->i_y * i_yscale) >> 6 ) * p_vout->output.i_width * 2;

    /* Draw until we reach the bottom of the subtitle */
    for( i_y = 0 ; i_y < i_height ; )
    {
        i_ytmp = i_y >> 6;
        i_y += i_yscale;

        /* Check whether we need to draw one line or more than one */
        if( i_ytmp + 1 >= ( i_y >> 6 ) )
        {
            /* Just one line : we precalculate i_y >> 6 */
            i_yreal = p_vout->output.i_width * 2 * i_ytmp;

            /* Draw until we reach the end of the line */
            for( i_x = i_width ; i_x ; )
            {
                /* Get the RLE part, then draw the line */
                i_color = *p_source & 0x3;

                switch( p_trsp[ i_color ] )
                {
                case 0x00:
                    i_x -= i_xscale * ( *p_source++ >> 2 );
                    break;

                case 0xff:
                    i_len = i_xscale * ( *p_source++ >> 2 );
                    memset( p_dest - 2 * ( i_x >> 6 ) + i_yreal,
                            p_clut16[ i_color ],
                            2 * ( ( i_len >> 6 ) + 1 ) );
                    i_x -= i_len;
                    break;

                default:
                    /* FIXME: we should do transparency */
                    i_len = i_xscale * ( *p_source++ >> 2 );
                    memset( p_dest - 2 * ( i_x >> 6 ) + i_yreal,
                            p_clut16[ i_color ],
                            2 * ( ( i_len >> 6 ) + 1 ) );
                    i_x -= i_len;
                    break;
                }

            }
        }
        else
        {
            i_yreal = p_vout->output.i_width * 2 * i_ytmp;
            i_ynext = p_vout->output.i_width * 2 * i_y >> 6;

            /* Draw until we reach the end of the line */
            for( i_x = i_width ; i_x ; )
            {
                /* Get the RLE part, then draw as many lines as needed */
                i_color = *p_source & 0x3;

                switch( p_trsp[ i_color ] )
                {
                case 0x00:
                    i_x -= i_xscale * ( *p_source++ >> 2 );
                    break;

                case 0xff:
                    i_len = i_xscale * ( *p_source++ >> 2 );
                    for( i_ytmp = i_yreal ; i_ytmp < i_ynext ;
                         i_ytmp += p_vout->output.i_width * 2 )
                    {
                        memset( p_dest - 2 * ( i_x >> 6 ) + i_ytmp,
                                p_clut16[ i_color ],
                                2 * ( ( i_len >> 6 ) + 1 ) );
                    }
                    i_x -= i_len;
                    break;

                default:
                    /* FIXME: we should do transparency */
                    i_len = i_xscale * ( *p_source++ >> 2 );
                    for( i_ytmp = i_yreal ; i_ytmp < i_ynext ;
                         i_ytmp += p_vout->output.i_width * 2 )
                    {
                        memset( p_dest - 2 * ( i_x >> 6 ) + i_ytmp,
                                p_clut16[ i_color ],
                                2 * ( ( i_len >> 6 ) + 1 ) );
                    }
                    i_x -= i_len;
                    break;
                }
            }
        }
    }

    break;

    default:
        intf_ErrMsg( "vout error: unknown chroma, can't render SPU" );
        break;
    }
}

