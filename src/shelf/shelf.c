/*
 * Compiz Fusion Shelf plugin
 *
 * Copyright (C) 2007  Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Author(s): 
 * Kristian Lyngstøl <kristian@bohemians.org>
 * Danny Baumann <maniac@opencompositing.org>
 *
 * Description:
 *
 * This plugin visually resizes a window to allow otherwise obtrusive
 * windows to be visible in a monitor-fashion. Use case: Anything with
 * progress bars, notification programs, etc.
 *
 * Todo: 
 *  - Check for XShape events
 *  - Handle input in a sane way
 *  - Correct damage handeling
 *  - Mouse-over?
 */

#include <compiz-core.h>
#include <X11/extensions/shape.h>
#include <X11/cursorfont.h>
#include <math.h>
#include "shelf_options.h"

typedef struct {
    Window     ipw;

    XRectangle *inputRects;
    int        nInputRects;
    int        inputRectOrdering;

    XRectangle *frameInputRects;
    int        frameNInputRects;
    int        frameInputRectOrdering;
} ShelfedWindowInfo;

typedef struct { 
    float scale;
    float targetScale;
    float steps;

    ShelfedWindowInfo *info;
} ShelfWindow;

typedef struct {
    int windowPrivateIndex;

    int grabIndex;
    Window grabbedWindow;

    Cursor moveCursor;

    Bool noLastPointer;
    int  lastPointerX;
    int  lastPointerY;

    PaintWindowProc        paintWindow;    
    DamageWindowRectProc   damageWindowRect;
    PreparePaintScreenProc preparePaintScreen;
    WindowMoveNotifyProc   windowMoveNotify;
} ShelfScreen;

typedef struct {
    int screenPrivateIndex;

    HandleEventProc handleEvent;
} ShelfDisplay;

static int displayPrivateIndex;

#define GET_SHELF_DISPLAY(d) \
    ((ShelfDisplay *) (d)->base.privates[displayPrivateIndex].ptr)
#define SHELF_DISPLAY(d) \
    ShelfDisplay *sd = GET_SHELF_DISPLAY (d)
#define GET_SHELF_SCREEN(s, sd) \
    ((ShelfScreen *) (s)->base.privates[(sd)->screenPrivateIndex].ptr)
#define SHELF_SCREEN(s) \
    ShelfScreen *ss = GET_SHELF_SCREEN (s, GET_SHELF_DISPLAY (s->display))
#define GET_SHELF_WINDOW(w, ss) \
    ((ShelfWindow *) (w)->base.privates[(ss)->windowPrivateIndex].ptr)
#define SHELF_WINDOW(w) \
    ShelfWindow *sw = GET_SHELF_WINDOW  (w,                  \
		      GET_SHELF_SCREEN  (w->screen,          \
		      GET_SHELF_DISPLAY (w->screen->display)))

#define SHELF_MIN_SIZE 50.0f // Minimum pixelsize a window can be scaled to

/* Checks if w is a ipw and returns the real window */
static CompWindow *
shelfGetRealWindow (CompWindow *w)
{
    CompWindow *rw;

    for (rw = w->screen->windows; rw; rw = rw->next)
    {
	SHELF_WINDOW (rw);
	if (sw->info && sw->info->ipw == w->id)
	    return rw;
    }
    return NULL;
}

static void
shelfSaveInputShape (CompWindow *w,
		     XRectangle **retRects,
		     int        *retCount,
		     int        *retOrdering)
{
    XRectangle *rects;
    int        count = 0, ordering;
    Display    *dpy = w->screen->display->display;

    rects = XShapeGetRectangles (dpy, w->id, ShapeInput, &count, &ordering);

    /* check if the returned shape exactly matches the window shape -
       if that is true, the window currently has no set input shape */
    if ((count == 1) &&
	(rects[0].x == -w->serverBorderWidth) &&
	(rects[0].y == -w->serverBorderWidth) &&
	(rects[0].width == (w->serverWidth + w->serverBorderWidth)) &&
	(rects[0].height == (w->serverHeight + w->serverBorderWidth)))
    {
	count = 0;
    }
    
    *retRects    = rects;
    *retCount    = count;
    *retOrdering = ordering;
}

/* Shape the input of the window when scaled.
 * Since the IPW will be dealing with the input, removing input
 * from the window entirely is a perfectly good solution. */
static void
shelfShapeInput (CompWindow *w)
{
    CompWindow *fw;
    Display    *dpy = w->screen->display->display;

    SHELF_WINDOW (w);

    /* save old shape */
    shelfSaveInputShape (w, &sw->info->inputRects,
			 &sw->info->nInputRects, &sw->info->inputRectOrdering);

    fw = findWindowAtDisplay (w->screen->display, w->frame);
    if (fw)
    {
	shelfSaveInputShape(fw, &sw->info->frameInputRects,
			    &sw->info->frameNInputRects,
			    &sw->info->frameInputRectOrdering);
    }
    else
    {
	sw->info->frameInputRects        = NULL;
	sw->info->frameNInputRects       = -1;
	sw->info->frameInputRectOrdering = 0;
    }

    /* clear shape */
    XShapeSelectInput (dpy, w->id, NoEventMask);
    XShapeCombineRectangles  (dpy, w->id, ShapeInput, 0, 0,
			      NULL, 0, ShapeSet, 0);
    
    if (w->frame)
	XShapeCombineRectangles  (dpy, w->frame, ShapeInput, 0, 0,
				  NULL, 0, ShapeSet, 0);

    XShapeSelectInput (dpy, w->id, ShapeNotify);
}

static void
shelfUnshapeInput (CompWindow *w)
{
    Display *dpy = w->screen->display->display;

    SHELF_WINDOW (w);

    if (sw->info->nInputRects)
    {
	XShapeCombineRectangles (dpy, w->id, ShapeInput, 0, 0,
				 sw->info->inputRects, sw->info->nInputRects,
				 ShapeSet, sw->info->inputRectOrdering);
    }
    else
    {
	XShapeCombineMask (dpy, w->id, ShapeInput, 0, 0, None, ShapeSet);
    }

    if (sw->info->frameNInputRects >= 0)
    {
	if (sw->info->frameInputRects)
	{
	    XShapeCombineRectangles (dpy, w->frame, ShapeInput, 0, 0,
				     sw->info->frameInputRects,
				     sw->info->frameNInputRects,
				     ShapeSet,
				     sw->info->frameInputRectOrdering);
	}
	else
	{
	    XShapeCombineMask (dpy, w->frame, ShapeInput, 0, 0, None, ShapeSet);
	}
    }
}

static void
shelfPreparePaintScreen (CompScreen *s,
			 int	    msSinceLastPaint)
{
    CompWindow *w;
    float      steps;

    SHELF_SCREEN (s);

    steps =  (float)msSinceLastPaint / (float)shelfGetAnimtime(s->display);

    if (steps < 0.005)
	steps = 0.005;

    /* FIXME: should only loop over all windows if at least one animation
       is running */
    for (w = s->windows; w; w = w->next)
	GET_SHELF_WINDOW (w, ss)->steps = steps;
    
    UNWRAP (ss, s, preparePaintScreen);
    (*s->preparePaintScreen) (s, msSinceLastPaint);
    WRAP (ss, s, preparePaintScreen, shelfPreparePaintScreen);
}

/* Adjust size and location of the input prevention window
 */
static void 
shelfAdjustIPW (CompWindow *w)
{
    XWindowChanges xwc;
    float          width, height;

    SHELF_WINDOW (w);

    if (!sw->info || !sw->info->ipw)
	return;

    width  = w->width + 2 * w->attrib.border_width +
	     w->input.left + w->input.right + 2.0f;
    width  *= sw->targetScale;
    height = w->height + 2 * w->attrib.border_width +
	     w->input.top + w->input.bottom + 2.0f;
    height *= sw->targetScale;

    xwc.x          = w->attrib.x - w->input.left;
    xwc.y          = w->attrib.y - w->input.top;
    xwc.width      = (int) width;
    xwc.height     = (int) height;
    xwc.stack_mode = Above;
    xwc.sibling    = w->id;

    XConfigureWindow (w->screen->display->display, sw->info->ipw,
		      CWSibling | CWStackMode | CWX | CWY | CWWidth | CWHeight,
		      &xwc);

    XMapWindow (w->screen->display->display, sw->info->ipw);
}

/* Create an input prevention window */
static void
shelfCreateIPW (CompWindow *w)
{
    Window               ipw;
    XSetWindowAttributes attrib;

    SHELF_WINDOW (w);

    if (!sw->info || sw->info->ipw)
	return;

    attrib.override_redirect = TRUE;
    ipw = XCreateWindow (w->screen->display->display,
			 w->screen->root,
			 w->serverX - w->input.left,
			 w->serverY - w->input.top,
			 w->serverWidth + w->input.left + w->input.right,
			 w->serverHeight + w->input.top + w->input.bottom,
			 0, CopyFromParent, InputOnly, CopyFromParent,
			 CWOverrideRedirect, &attrib);

    sw->info->ipw = ipw;
}

static Bool
shelfHandleShelfInfo (CompWindow *w)
{
    SHELF_WINDOW (w);

    if (sw->targetScale == 1.0f && sw->info)
    {
	if (sw->info->ipw)
	    XDestroyWindow (w->screen->display->display, sw->info->ipw);

	shelfUnshapeInput (w);

	free (sw->info);
	sw->info = NULL;

	return FALSE;
    }
    else if (sw->targetScale != 1.0f && !sw->info)
    {
	sw->info = calloc (1, sizeof (ShelfedWindowInfo));
	if (!sw->info)
	    return FALSE;

	shelfShapeInput (w);
	shelfCreateIPW (w);
    }

    return TRUE;
}

/* Sets the scale level and adjust the shape */
static void
shelfScaleWindow (CompWindow *w,
		  float      scale)
{
    SHELF_WINDOW (w);

    if (w->wmType & (CompWindowTypeDesktopMask | CompWindowTypeDockMask))
	return;

    sw->targetScale = MIN (scale, 1.0f);

    if ((float) w->width * sw->targetScale < SHELF_MIN_SIZE)
	sw->targetScale = SHELF_MIN_SIZE / (float) w->width;

    if (!shelfHandleShelfInfo (w))
	return;

    shelfAdjustIPW (w);

    damageScreen (w->screen);
}

/* Binding for toggle mode. 
 * Toggles through three preset scale levels, 
 * currently hard coded to 1.0f (no scale), 0.5f and 0.25f.
 */
static Bool
shelfTrigger (CompDisplay     *d,
	      CompAction      *action,
	      CompActionState state,
	      CompOption      *option,
	      int             nOption)
{
    CompWindow *w = findWindowAtDisplay (d, d->activeWindow);
    if (!w)
	return TRUE;

    SHELF_WINDOW (w);

    if (sw->targetScale > 0.5f)
	shelfScaleWindow (w, 0.5f);
    else if (sw->targetScale <= 0.5f && sw->targetScale > 0.25)
	shelfScaleWindow (w, 0.25f);
    else 
	shelfScaleWindow (w, 1.0f);

    return TRUE;
}

/* Returns the ratio to multiply by to get a window that's 1/ration the
 * size of the screen.
 */
static inline float
shelfRat (CompWindow *w,
	  float      ratio)
{
    float winHeight    = (float) w->height;
    float winWidth     = (float) w->width;
    float screenHeight = (float) w->screen->height;
    float screenWidth  = (float) w->screen->width;
    float ret;

    if (winHeight / screenHeight < winWidth / screenWidth)
	ret = screenWidth / winWidth;
    else
	ret = screenHeight / winHeight;

    return ret / ratio;
}

static Bool
shelfTriggerScreen (CompDisplay *d,
		    CompAction *action,
		    CompActionState state,
		    CompOption      *option,
		    int             nOption)
{
    CompWindow *w = findWindowAtDisplay (d, d->activeWindow);
    if (!w)
	return TRUE;

    SHELF_WINDOW (w);

    /* FIXME: better should save calculated ratio and reuse it */
    if (sw->targetScale > shelfRat (w, 2.0f))
	shelfScaleWindow (w, shelfRat (w, 2.0f));
    else if (sw->targetScale <= shelfRat (w, 2.0f) && 
	     sw->targetScale > shelfRat (w, 3.0f))
	shelfScaleWindow (w, shelfRat (w, 3.0f));
    else if (sw->targetScale <= shelfRat (w, 3.0f) && 
	     sw->targetScale > shelfRat (w, 6.0f))
	shelfScaleWindow (w, shelfRat (w, 6.0f));
    else 
	shelfScaleWindow (w, 1.0f);

    return TRUE;
}

/* shelfInc and shelfDec are matcing functions and bindings;
 * They increase and decrease the scale factor by 'interval'.
 */
static Bool
shelfInc (CompDisplay     *d,
	  CompAction      *action,
	  CompActionState state,
	  CompOption      *option,
	  int             nOption)
{
    CompWindow *w = findWindowAtDisplay (d, d->activeWindow);
    if (!w)
	return TRUE;

    SHELF_WINDOW (w);

    shelfScaleWindow (w, sw->targetScale / shelfGetInterval (d));

    return TRUE;
}

static Bool
shelfDec (CompDisplay     *d,
	  CompAction      *action,
	  CompActionState state,
	  CompOption      *option,
	  int             nOption)
{
    CompWindow *w = findWindowAtDisplay (d, d->activeWindow);
    if (!w)
	return TRUE;

    SHELF_WINDOW (w);

    shelfScaleWindow (w, sw->targetScale * shelfGetInterval (d));

    return TRUE;
}

static void
handleButtonPress (CompWindow *w)
{
    CompScreen *s = w->screen;

    SHELF_SCREEN (s);

    if (!otherScreenGrabExist (s, "shelf", 0))
    {
	moveInputFocusToWindow (w);
	ss->grabbedWindow = w->id;
	ss->grabIndex = pushScreenGrab (s, ss->moveCursor, "shelf");
    }
}

static void
handleMotionEvent (CompDisplay *d, XEvent *event)
{
    CompScreen *s;
    CompWindow *w;
    int        x,y;

    s = findScreenAtDisplay (d, event->xmotion.root);
    if (!s)
	return;

    SHELF_SCREEN (s);

    if (!ss->grabIndex)
	return;

    w = findWindowAtScreen (s, ss->grabbedWindow);
    if (!w)
	return;

    x = event->xmotion.x_root;
    y = event->xmotion.y_root;

    if (ss->noLastPointer)
    {
	ss->noLastPointer = FALSE;
	ss->lastPointerX = x;
	ss->lastPointerY = y;
	return;
    }

    moveWindow (w,
		-ss->lastPointerX + x,
		-ss->lastPointerY + y,
		TRUE, FALSE);
    syncWindowPosition (w);

    ss->lastPointerX = event->xmotion.x_root;
    ss->lastPointerY = event->xmotion.y_root;
}

static void
handleButtonRelease (CompWindow *w)
{
    CompScreen *s = w->screen;

    SHELF_SCREEN (s);

    ss->grabbedWindow = None;
    if (ss->grabIndex)
    {
	ss->noLastPointer = TRUE;
	ss->lastPointerX  = 0;
	ss->lastPointerY  = 0;

	/* FIXME: shouldn't we better activateWindow() here? */
	moveInputFocusToWindow (w);
	removeScreenGrab (s, ss->grabIndex, NULL);
	ss->grabIndex = 0;
	ss->grabbedWindow = 0;
    }
}

static CompWindow *
shelfFindRealWindowID (CompDisplay *d,
		       Window      wid)
{
    CompWindow *orig;

    orig = findWindowAtDisplay (d, wid);
    if (!orig)
	return NULL;

    return shelfGetRealWindow (orig);
}

static void
shelfHandleEvent (CompDisplay *d,
		  XEvent      *event)
{
    CompWindow *w;
    CompScreen *s;

    SHELF_DISPLAY (d);

    switch (event->type)
    {
	case ButtonPress:
	    w = shelfFindRealWindowID (d, event->xbutton.window);
	    if (w)
		handleButtonPress (w);
	    break;
	case ButtonRelease:
	    s = findScreenAtDisplay (d, event->xbutton.root);
	    if (s)
	    {
		SHELF_SCREEN (s);
		w = findWindowAtDisplay (d, ss->grabbedWindow);
		if (w)
		    handleButtonRelease (w);
	    }
	    break;
	case MotionNotify:
	    handleMotionEvent (d, event);
	    break;
    }

    UNWRAP (sd, d, handleEvent);
    (*d->handleEvent) (d, event);
    WRAP (sd, d, handleEvent, shelfHandleEvent);
}

/* The window was damaged, adjust the damage to fit the actual area we
 * care about.
 *
 * FIXME:
 * This should not cause total screen damage, but only adjust the rect
 * correctly.
 */
static Bool
shelfDamageWindowRect (CompWindow *w,
                       Bool       initial,
                       BoxPtr     rect)
{
    Bool status = FALSE;
    SHELF_SCREEN (w->screen);
    SHELF_WINDOW (w);

    if (sw->scale != 1.0f)
    {
	damageScreen (w->screen);
	status = TRUE;
    }

    UNWRAP (ss, w->screen, damageWindowRect);
    status |= (*w->screen->damageWindowRect) (w, initial, rect);
    WRAP (ss, w->screen, damageWindowRect, shelfDamageWindowRect);

    return status;
}

/* Scale the window if it is supposed to be scaled.
 * Translate into place.
 *
 * FIXME: Merge the two translations.
 */
static Bool
shelfPaintWindow (CompWindow		    *w,
		  const WindowPaintAttrib   *attrib,
		  const CompTransform	    *transform,
		  Region		    region,
		  unsigned int		    mask)
{
    Bool       status;
    CompScreen *s = w->screen;

    SHELF_SCREEN (s);
    SHELF_WINDOW (w);

    if (sw->targetScale != sw->scale && sw->steps)
    {
	sw->scale += (float) sw->steps * (sw->targetScale - sw->scale);
	if (fabsf (sw->targetScale - sw->scale) < 0.005)
	    sw->scale = sw->targetScale;
    }

    if (sw->scale != 1.0f)
    {
	CompTransform mTransform = *transform;
	int           xOrigin, yOrigin;

	xOrigin = w->attrib.x - w->input.left;
	yOrigin = w->attrib.y - w->input.top;
	
	matrixTranslate (&mTransform, xOrigin, yOrigin,  0);
	matrixScale (&mTransform, sw->scale, sw->scale, 0);
	matrixTranslate (&mTransform, -xOrigin, -yOrigin,  0);
	
	mask |= PAINT_WINDOW_TRANSFORMED_MASK;

	/* FIXME: should better use DonePaintScreen for that */
	if (sw->scale != sw->targetScale)
	    addWindowDamage (w);	    

	UNWRAP (ss, s, paintWindow);
	status = (*s->paintWindow) (w, attrib, &mTransform, region, mask);
	WRAP (ss, s, paintWindow, shelfPaintWindow);
    }
    else
    {
	UNWRAP (ss, s, paintWindow);
	status = (*s->paintWindow) (w, attrib, transform, region, mask);
	WRAP (ss, s, paintWindow, shelfPaintWindow);
    }

    return status;
}

static void
shelfWindowMoveNotify (CompWindow *w,
		       int        dx,
		       int        dy,
		       Bool       immediate)
{
    SHELF_SCREEN (w->screen);
    SHELF_WINDOW (w);

    if (sw->targetScale != 1.00f)
	shelfAdjustIPW (w);

    UNWRAP (ss, w->screen, windowMoveNotify);
    (*w->screen->windowMoveNotify) (w, dx, dy, immediate);
    WRAP (ss, w->screen, windowMoveNotify, shelfWindowMoveNotify);
}

/* Configuration, initialization, boring stuff. --------------------- */
static Bool
shelfInitScreen (CompPlugin *p,
		 CompScreen *s)
{
    ShelfScreen *ss;

    SHELF_DISPLAY (s->display);

    ss = malloc (sizeof (ShelfScreen));
    if (!ss)
	return FALSE; // fixme: error message.

    ss->windowPrivateIndex = allocateWindowPrivateIndex (s);
    if (ss->windowPrivateIndex < 0)
    {
	free (ss);
	return FALSE;
    }

    ss->moveCursor = XCreateFontCursor (s->display->display, XC_fleur);

    ss->lastPointerX  = 0;
    ss->lastPointerY  = 0;
    ss->noLastPointer = TRUE;

    WRAP (ss, s, preparePaintScreen, shelfPreparePaintScreen);
    WRAP (ss, s, paintWindow, shelfPaintWindow); 
    WRAP (ss, s, damageWindowRect, shelfDamageWindowRect);
    WRAP (ss, s, windowMoveNotify, shelfWindowMoveNotify);

    s->base.privates[sd->screenPrivateIndex].ptr = ss;

    return TRUE; 
}

static void
shelfFiniScreen (CompPlugin *p,
		 CompScreen *s)
{
    SHELF_SCREEN (s);

    UNWRAP (ss, s, preparePaintScreen);
    UNWRAP (ss, s, paintWindow);
    UNWRAP (ss, s, damageWindowRect);
    UNWRAP (ss, s, windowMoveNotify);

    freeWindowPrivateIndex (s, ss->windowPrivateIndex);

    if (ss->moveCursor)
	XFreeCursor (s->display->display, ss->moveCursor);

    free (ss);
}

static Bool
shelfInitDisplay (CompPlugin  *p,
		  CompDisplay *d)
{
    ShelfDisplay *sd;

    if (!checkPluginABI ("core", CORE_ABIVERSION))
	return FALSE;

    if (!d->shapeExtension)
    {
	compLogMessage (d, "shelf", CompLogLevelError,
			"No Shape extension found. Shelfing not possible.\n");
	return FALSE;
    }

    sd = malloc (sizeof (ShelfDisplay));
    if (!sd)
	return FALSE;

    sd->screenPrivateIndex = allocateScreenPrivateIndex (d); 
    if (sd->screenPrivateIndex < 0)
    {
	free (sd);
	return FALSE;
    }

    shelfSetTriggerKeyInitiate (d, shelfTrigger);
    shelfSetTriggerscreenKeyInitiate (d, shelfTriggerScreen);
    shelfSetIncButtonInitiate (d, shelfInc);
    shelfSetDecButtonInitiate (d, shelfDec);

    WRAP (sd, d, handleEvent, shelfHandleEvent);
    
    d->base.privates[displayPrivateIndex].ptr = sd;

    return TRUE;
}

static void
shelfFiniDisplay (CompPlugin  *p,
		  CompDisplay *d)
{
    SHELF_DISPLAY (d);

    freeScreenPrivateIndex (d, sd->screenPrivateIndex);

    UNWRAP (sd, d, handleEvent);

    free (sd);
}

static void
shelfFini (CompPlugin *p)
{
    freeDisplayPrivateIndex (displayPrivateIndex);
}

static Bool
shelfInit (CompPlugin *p)
{
    displayPrivateIndex = allocateDisplayPrivateIndex ();
    if (displayPrivateIndex < 0)
	return FALSE;

    return TRUE;
}

static Bool
shelfInitWindow (CompPlugin *p,
		 CompWindow *w)
{
    ShelfWindow *sw;

    SHELF_SCREEN (w->screen);

    sw = malloc (sizeof (ShelfWindow));
    if (!sw)
	return FALSE;

    sw->scale       = 1.0f;
    sw->targetScale = 1.0f;

    sw->info = NULL;

    w->base.privates[ss->windowPrivateIndex].ptr = sw;

    return TRUE;
}

static void
shelfFiniWindow (CompPlugin *p,
		 CompWindow *w)
{
    SHELF_WINDOW (w);

    if (sw->info)
    {
	sw->targetScale = 1.0f;
	/* implicitly frees sw->info */
	shelfHandleShelfInfo (w);
    }

    free (sw);
}

static CompBool
shelfInitObject (CompPlugin *p,
		 CompObject *o)
{
    static InitPluginObjectProc dispTab[] = {
	    (InitPluginObjectProc) 0, /* InitCore */
	    (InitPluginObjectProc) shelfInitDisplay,
	    (InitPluginObjectProc) shelfInitScreen, 
	    (InitPluginObjectProc) shelfInitWindow
    };

    RETURN_DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), TRUE, (p, o));
}

static void
shelfFiniObject (CompPlugin *p,
		 CompObject *o)
{
    static FiniPluginObjectProc dispTab[] = {
	(FiniPluginObjectProc) 0, /* InitCore */
	(FiniPluginObjectProc) shelfFiniDisplay,
	(FiniPluginObjectProc) shelfFiniScreen, 
	(FiniPluginObjectProc) shelfFiniWindow
    };

    DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), (p, o));
}

CompPluginVTable shelfVTable = {
    "shelf",
    0,
    shelfInit,
    shelfFini,
    shelfInitObject,
    shelfFiniObject,
    0,
    0
};

CompPluginVTable*
getCompPluginInfo (void)
{
    return &shelfVTable;
}