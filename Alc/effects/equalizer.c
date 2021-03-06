/**
 * OpenAL cross platform audio library
 * Copyright (C) 2013 by Mike Gorchak
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include "alMain.h"
#include "alFilter.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"


/*  The document  "Effects Extension Guide.pdf"  says that low and high  *
 *  frequencies are cutoff frequencies. This is not fully correct, they  *
 *  are corner frequencies for low and high shelf filters. If they were  *
 *  just cutoff frequencies, there would be no need in cutoff frequency  *
 *  gains, which are present.  Documentation for  "Creative Proteus X2"  *
 *  software describes  4-band equalizer functionality in a much better  *
 *  way.  This equalizer seems  to be a predecessor  of  OpenAL  4-band  *
 *  equalizer.  With low and high  shelf filters  we are able to cutoff  *
 *  frequencies below and/or above corner frequencies using attenuation  *
 *  gains (below 1.0) and amplify all low and/or high frequencies using  *
 *  gains above 1.0.                                                     *
 *                                                                       *
 *     Low-shelf       Low Mid Band      High Mid Band     High-shelf    *
 *      corner            center             center          corner      *
 *     frequency        frequency          frequency       frequency     *
 *    50Hz..800Hz     200Hz..3000Hz      1000Hz..8000Hz  4000Hz..16000Hz *
 *                                                                       *
 *          |               |                  |               |         *
 *          |               |                  |               |         *
 *   B -----+            /--+--\            /--+--\            +-----    *
 *   O      |\          |   |   |          |   |   |          /|         *
 *   O      | \        -    |    -        -    |    -        / |         *
 *   S +    |  \      |     |     |      |     |     |      /  |         *
 *   T      |   |    |      |      |    |      |      |    |   |         *
 * ---------+---------------+------------------+---------------+-------- *
 *   C      |   |    |      |      |    |      |      |    |   |         *
 *   U -    |  /      |     |     |      |     |     |      \  |         *
 *   T      | /        -    |    -        -    |    -        \ |         *
 *   O      |/          |   |   |          |   |   |          \|         *
 *   F -----+            \--+--/            \--+--/            +-----    *
 *   F      |               |                  |               |         *
 *          |               |                  |               |         *
 *                                                                       *
 * Gains vary from 0.126 up to 7.943, which means from -18dB attenuation *
 * up to +18dB amplification. Band width varies from 0.01 up to 1.0 in   *
 * octaves for two mid bands.                                            *
 *                                                                       *
 * Implementation is based on the "Cookbook formulae for audio EQ biquad *
 * filter coefficients" by Robert Bristow-Johnson                        *
 * http://www.musicdsp.org/files/Audio-EQ-Cookbook.txt                   */


/* The maximum number of sample frames per update. */
#define MAX_UPDATE_SAMPLES 256

typedef struct ALequalizerState {
    DERIVE_FROM_TYPE(ALeffectState);

    /* Effect gains for each channel */
    ALfloat Gain[MAX_EFFECT_CHANNELS][MAX_OUTPUT_CHANNELS];

    /* Effect parameters */
    //ALfilterState filter[4][MAX_EFFECT_CHANNELS];
    ALfilterState filter[8][MAX_EFFECT_CHANNELS];

    //ALfloat SampleBuffer[4][MAX_EFFECT_CHANNELS][MAX_UPDATE_SAMPLES];
    ALfloat SampleBuffer[8][MAX_EFFECT_CHANNELS][MAX_UPDATE_SAMPLES];
} ALequalizerState;

static ALvoid ALequalizerState_Destruct(ALequalizerState *state);
static ALboolean ALequalizerState_deviceUpdate(ALequalizerState *state, ALCdevice *device);
static ALvoid ALequalizerState_update(ALequalizerState *state, const ALCdevice *device, const ALeffectslot *slot, const ALeffectProps *props);
static ALvoid ALequalizerState_process(ALequalizerState *state, ALuint SamplesToDo, const ALfloat (*SamplesIn)[BUFFERSIZE], ALfloat (*SamplesOut)[BUFFERSIZE], ALuint NumChannels);
DECLARE_DEFAULT_ALLOCATORS(ALequalizerState)

DEFINE_ALEFFECTSTATE_VTABLE(ALequalizerState);

extern int GetFilterType(int band);

static void ALequalizerState_Construct(ALequalizerState *state)
{
    int it, ft;

    ALeffectState_Construct(STATIC_CAST(ALeffectState, state));
    SET_VTABLE2(ALequalizerState, ALeffectState, state);

    /* Initialize sample history only on filter creation to avoid */
    /* sound clicks if filter settings were changed in runtime.   */
    //for(it = 0; it < 4; it++)
    for(it = 0; it < 8; it++)
    {
        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
            ALfilterState_clear(&state->filter[it][ft]);
    }
}

static ALvoid ALequalizerState_Destruct(ALequalizerState *state)
{
    ALeffectState_Destruct(STATIC_CAST(ALeffectState,state));
}

static ALboolean ALequalizerState_deviceUpdate(ALequalizerState *UNUSED(state), ALCdevice *UNUSED(device))
{
    return AL_TRUE;
}

static ALvoid ALequalizerState_update(ALequalizerState *state, const ALCdevice *device, const ALeffectslot *slot, const ALeffectProps *props)
{
    ALfloat frequency = (ALfloat)device->Frequency;
    ALfloat gain, freq_mult;
    ALuint i;
    int		filtertype;

    STATIC_CAST(ALeffectState,state)->OutBuffer = device->FOAOut.Buffer;
    STATIC_CAST(ALeffectState,state)->OutChannels = device->FOAOut.NumChannels;
    for(i = 0;i < MAX_EFFECT_CHANNELS;i++)
        ComputeFirstOrderGains(device->FOAOut, IdentityMatrixf.m[i],
                               slot->Params.Gain, state->Gain[i]);

    /* Calculate coefficients for the each type of filter. Note that the shelf
     * filters' gain is for the reference frequency, which is the centerpoint
     * of the transition band.
     */
    /*gain = maxf(sqrtf(props->Equalizer.LowGain), 0.0625f);
    freq_mult = props->Equalizer.LowCutoff/frequency;
    ALfilterState_setParams(&state->filter[0][0], ALfilterType_LowShelf,
        gain, freq_mult, calc_rcpQ_from_slope(gain, 0.75f)
    );

    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
    {
        state->filter[0][i].b0 = state->filter[0][0].b0;
        state->filter[0][i].b1 = state->filter[0][0].b1;
        state->filter[0][i].b2 = state->filter[0][0].b2;
        state->filter[0][i].a1 = state->filter[0][0].a1;
        state->filter[0][i].a2 = state->filter[0][0].a2;
    }

    gain = maxf(props->Equalizer.Mid1Gain, 0.0625f);
    freq_mult = props->Equalizer.Mid1Center/frequency;
    ALfilterState_setParams(&state->filter[1][0], ALfilterType_Peaking,
        gain, freq_mult, calc_rcpQ_from_bandwidth(
            freq_mult, props->Equalizer.Mid1Width
        )
    );
    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
    {
        state->filter[1][i].b0 = state->filter[1][0].b0;
        state->filter[1][i].b1 = state->filter[1][0].b1;
        state->filter[1][i].b2 = state->filter[1][0].b2;
        state->filter[1][i].a1 = state->filter[1][0].a1;
        state->filter[1][i].a2 = state->filter[1][0].a2;
    }

    gain = maxf(props->Equalizer.Mid2Gain, 0.0625f);
    freq_mult = props->Equalizer.Mid2Center/frequency;
    ALfilterState_setParams(&state->filter[2][0], ALfilterType_Peaking,
        gain, freq_mult, calc_rcpQ_from_bandwidth(
            freq_mult, props->Equalizer.Mid2Width
        )
    );
    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
    {
        state->filter[2][i].b0 = state->filter[2][0].b0;
        state->filter[2][i].b1 = state->filter[2][0].b1;
        state->filter[2][i].b2 = state->filter[2][0].b2;
        state->filter[2][i].a1 = state->filter[2][0].a1;
        state->filter[2][i].a2 = state->filter[2][0].a2;
    }

    gain = maxf(sqrtf(props->Equalizer.HighGain), 0.0625f);
    freq_mult = props->Equalizer.HighCutoff/frequency;
    ALfilterState_setParams(&state->filter[3][0], ALfilterType_HighShelf,
        gain, freq_mult, calc_rcpQ_from_slope(gain, 0.75f)
    );
    for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
    {
        state->filter[3][i].b0 = state->filter[3][0].b0;
        state->filter[3][i].b1 = state->filter[3][0].b1;
        state->filter[3][i].b2 = state->filter[3][0].b2;
        state->filter[3][i].a1 = state->filter[3][0].a1;
        state->filter[3][i].a2 = state->filter[3][0].a2;
    }*/
    filtertype = GetFilterType(0);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid0Gain);
				freq_mult = props->Equalizer.Mid0Center/frequency;
				ALfilterState_setParams(&state->filter[0][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid0Gain;
				freq_mult = props->Equalizer.Mid0Center/frequency;
				ALfilterState_setParams(&state->filter[0][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid0Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[0][i].b0 = state->filter[0][0].b0;
        state->filter[0][i].b1 = state->filter[0][0].b1;
        state->filter[0][i].b2 = state->filter[0][0].b2;
        state->filter[0][i].a1 = state->filter[0][0].a1;
        state->filter[0][i].a2 = state->filter[0][0].a2;
	}

	filtertype = GetFilterType(1);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid1Gain);
				freq_mult = props->Equalizer.Mid1Center/frequency;
				ALfilterState_setParams(&state->filter[1][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid1Gain;
				freq_mult = props->Equalizer.Mid1Center/frequency;
				ALfilterState_setParams(&state->filter[1][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid1Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[1][i].b0 = state->filter[1][0].b0;
        state->filter[1][i].b1 = state->filter[1][0].b1;
        state->filter[1][i].b2 = state->filter[1][0].b2;
        state->filter[1][i].a1 = state->filter[1][0].a1;
        state->filter[1][i].a2 = state->filter[1][0].a2;
	}

	filtertype = GetFilterType(2);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid2Gain);
				freq_mult = props->Equalizer.Mid2Center/frequency;
				ALfilterState_setParams(&state->filter[2][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid2Gain;
				freq_mult = props->Equalizer.Mid2Center/frequency;
				ALfilterState_setParams(&state->filter[2][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid2Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[2][i].b0 = state->filter[2][0].b0;
        state->filter[2][i].b1 = state->filter[2][0].b1;
        state->filter[2][i].b2 = state->filter[2][0].b2;
        state->filter[2][i].a1 = state->filter[2][0].a1;
        state->filter[2][i].a2 = state->filter[2][0].a2;
	}

	filtertype = GetFilterType(3);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid3Gain);
				freq_mult = props->Equalizer.Mid3Center/frequency;
				ALfilterState_setParams(&state->filter[3][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid3Gain;
				freq_mult = props->Equalizer.Mid3Center/frequency;
				ALfilterState_setParams(&state->filter[3][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid3Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[3][i].b0 = state->filter[3][0].b0;
        state->filter[3][i].b1 = state->filter[3][0].b1;
        state->filter[3][i].b2 = state->filter[3][0].b2;
        state->filter[3][i].a1 = state->filter[3][0].a1;
        state->filter[3][i].a2 = state->filter[3][0].a2;
	}

	/******************new*******************/
	filtertype = GetFilterType(4);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid4Gain);
				freq_mult = props->Equalizer.Mid4Center/frequency;
				ALfilterState_setParams(&state->filter[4][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid4Gain;
				freq_mult = props->Equalizer.Mid4Center/frequency;
				ALfilterState_setParams(&state->filter[4][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid4Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[4][i].b0 = state->filter[4][0].b0;
        state->filter[4][i].b1 = state->filter[4][0].b1;
        state->filter[4][i].b2 = state->filter[4][0].b2;
        state->filter[4][i].a1 = state->filter[4][0].a1;
        state->filter[4][i].a2 = state->filter[4][0].a2;
	}

	filtertype = GetFilterType(5);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid5Gain);
				freq_mult = props->Equalizer.Mid5Center/frequency;
				ALfilterState_setParams(&state->filter[5][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid5Gain;
				freq_mult = props->Equalizer.Mid5Center/frequency;
				ALfilterState_setParams(&state->filter[5][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid5Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[5][i].b0 = state->filter[5][0].b0;
        state->filter[5][i].b1 = state->filter[5][0].b1;
        state->filter[5][i].b2 = state->filter[5][0].b2;
        state->filter[5][i].a1 = state->filter[5][0].a1;
        state->filter[5][i].a2 = state->filter[5][0].a2;
	}

	filtertype = GetFilterType(6);
    switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid6Gain);
				freq_mult = props->Equalizer.Mid6Center/frequency;
				ALfilterState_setParams(&state->filter[6][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid6Gain;
				freq_mult = props->Equalizer.Mid6Center/frequency;
				ALfilterState_setParams(&state->filter[6][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid6Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[6][i].b0 = state->filter[6][0].b0;
        state->filter[6][i].b1 = state->filter[6][0].b1;
        state->filter[6][i].b2 = state->filter[6][0].b2;
        state->filter[6][i].a1 = state->filter[6][0].a1;
        state->filter[6][i].a2 = state->filter[6][0].a2;
	}

	filtertype = GetFilterType(7);
	switch( filtertype )
	{
		case ALfilterType_HighShelf:
		case ALfilterType_LowShelf:
		case ALfilterType_LowPass:
		case ALfilterType_HighPass:
				gain = sqrtf(props->Equalizer.Mid7Gain);
				freq_mult = props->Equalizer.Mid7Center/frequency;
				ALfilterState_setParams(&state->filter[7][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_slope(gain, 0.75f));
				break;

		default:
				gain = props->Equalizer.Mid7Gain;
				freq_mult = props->Equalizer.Mid7Center/frequency;
				ALfilterState_setParams(&state->filter[7][0],
														filtertype,
														gain,
														freq_mult,
														calc_rcpQ_from_bandwidth(freq_mult, props->Equalizer.Mid7Width));
				break;
	}
	for(i = 1;i < MAX_EFFECT_CHANNELS;i++)
	{
        state->filter[7][i].b0 = state->filter[7][0].b0;
        state->filter[7][i].b1 = state->filter[7][0].b1;
        state->filter[7][i].b2 = state->filter[7][0].b2;
        state->filter[7][i].a1 = state->filter[7][0].a1;
        state->filter[7][i].a2 = state->filter[7][0].a2;
	}
}

static ALvoid ALequalizerState_process(ALequalizerState *state, ALuint SamplesToDo, const ALfloat (*SamplesIn)[BUFFERSIZE], ALfloat (*SamplesOut)[BUFFERSIZE], ALuint NumChannels)
{
    ALfloat (*Samples)[MAX_EFFECT_CHANNELS][MAX_UPDATE_SAMPLES] = state->SampleBuffer;
    ALuint it, kt, ft;
    ALuint base;

    for(base = 0;base < SamplesToDo;)
    {
        ALuint td = minu(MAX_UPDATE_SAMPLES, SamplesToDo-base);

        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
            ALfilterState_process(&state->filter[0][ft], Samples[0][ft], &SamplesIn[ft][base], td);
        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
            ALfilterState_process(&state->filter[1][ft], Samples[1][ft], Samples[0][ft], td);
        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
            ALfilterState_process(&state->filter[2][ft], Samples[2][ft], Samples[1][ft], td);
        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
            ALfilterState_process(&state->filter[3][ft], Samples[3][ft], Samples[2][ft], td);
		/******************new*******************/
		for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
			ALfilterState_process(&state->filter[4][ft], Samples[4][ft], Samples[3][ft], td);
		for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
			ALfilterState_process(&state->filter[5][ft], Samples[5][ft], Samples[4][ft], td);
		for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
			ALfilterState_process(&state->filter[6][ft], Samples[6][ft], Samples[5][ft], td);
		for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
			ALfilterState_process(&state->filter[7][ft], Samples[7][ft], Samples[6][ft], td);

        for(ft = 0;ft < MAX_EFFECT_CHANNELS;ft++)
        {
            for(kt = 0;kt < NumChannels;kt++)
            {
                ALfloat gain = state->Gain[ft][kt];
                if(!(fabsf(gain) > GAIN_SILENCE_THRESHOLD))
                    continue;

                for(it = 0;it < td;it++)
                    //SamplesOut[kt][base+it] += gain * Samples[3][ft][it];
                	SamplesOut[kt][base+it] += gain * Samples[7][ft][it];
            }
        }

        base += td;
    }
}


typedef struct ALequalizerStateFactory {
    DERIVE_FROM_TYPE(ALeffectStateFactory);
} ALequalizerStateFactory;

ALeffectState *ALequalizerStateFactory_create(ALequalizerStateFactory *UNUSED(factory))
{
    ALequalizerState *state;

    NEW_OBJ0(state, ALequalizerState)();
    if(!state) return NULL;

    return STATIC_CAST(ALeffectState, state);
}

DEFINE_ALEFFECTSTATEFACTORY_VTABLE(ALequalizerStateFactory);

ALeffectStateFactory *ALequalizerStateFactory_getFactory(void)
{
    static ALequalizerStateFactory EqualizerFactory = { { GET_VTABLE2(ALequalizerStateFactory, ALeffectStateFactory) } };

    return STATIC_CAST(ALeffectStateFactory, &EqualizerFactory);
}


void ALequalizer_setParami(ALeffect *UNUSED(effect), ALCcontext *context, ALenum UNUSED(param), ALint UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
void ALequalizer_setParamiv(ALeffect *effect, ALCcontext *context, ALenum param, const ALint *vals)
{
    ALequalizer_setParami(effect, context, param, vals[0]);
}
void ALequalizer_setParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    ALeffectProps *props = &effect->Props;
    switch(param)
    {
        /*case AL_EQUALIZER_LOW_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_LOW_GAIN && val <= AL_EQUALIZER_MAX_LOW_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.LowGain = val;
            break;

        case AL_EQUALIZER_LOW_CUTOFF:
            if(!(val >= AL_EQUALIZER_MIN_LOW_CUTOFF && val <= AL_EQUALIZER_MAX_LOW_CUTOFF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.LowCutoff = val;
            break;

        case AL_EQUALIZER_MID1_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_MID1_GAIN && val <= AL_EQUALIZER_MAX_MID1_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Gain = val;
            break;

        case AL_EQUALIZER_MID1_CENTER:
            if(!(val >= AL_EQUALIZER_MIN_MID1_CENTER && val <= AL_EQUALIZER_MAX_MID1_CENTER))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Center = val;
            break;

        case AL_EQUALIZER_MID1_WIDTH:
            if(!(val >= AL_EQUALIZER_MIN_MID1_WIDTH && val <= AL_EQUALIZER_MAX_MID1_WIDTH))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid1Width = val;
            break;

        case AL_EQUALIZER_MID2_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_MID2_GAIN && val <= AL_EQUALIZER_MAX_MID2_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Gain = val;
            break;

        case AL_EQUALIZER_MID2_CENTER:
            if(!(val >= AL_EQUALIZER_MIN_MID2_CENTER && val <= AL_EQUALIZER_MAX_MID2_CENTER))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Center = val;
            break;

        case AL_EQUALIZER_MID2_WIDTH:
            if(!(val >= AL_EQUALIZER_MIN_MID2_WIDTH && val <= AL_EQUALIZER_MAX_MID2_WIDTH))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.Mid2Width = val;
            break;

        case AL_EQUALIZER_HIGH_GAIN:
            if(!(val >= AL_EQUALIZER_MIN_HIGH_GAIN && val <= AL_EQUALIZER_MAX_HIGH_GAIN))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.HighGain = val;
            break;

        case AL_EQUALIZER_HIGH_CUTOFF:
            if(!(val >= AL_EQUALIZER_MIN_HIGH_CUTOFF && val <= AL_EQUALIZER_MAX_HIGH_CUTOFF))
                SET_ERROR_AND_RETURN(context, AL_INVALID_VALUE);
            props->Equalizer.HighCutoff = val;
            break;*/
    case AL_EQUALIZER_MID0_GAIN:
			props->Equalizer.Mid0Gain = val;
			break;
		case AL_EQUALIZER_MID0_CENTER:
			props->Equalizer.Mid0Center = val;
			break;
		case AL_EQUALIZER_MID0_WIDTH:
			props->Equalizer.Mid0Width = val;
			break;

		case AL_EQUALIZER_MID1_GAIN:
			props->Equalizer.Mid1Gain = val;
			break;
		case AL_EQUALIZER_MID1_CENTER:
			props->Equalizer.Mid1Center = val;
			break;
		case AL_EQUALIZER_MID1_WIDTH:
			props->Equalizer.Mid1Width = val;
			break;

		case AL_EQUALIZER_MID2_GAIN:
			props->Equalizer.Mid2Gain = val;
			break;
		case AL_EQUALIZER_MID2_CENTER:
			props->Equalizer.Mid2Center = val;
			break;
		case AL_EQUALIZER_MID2_WIDTH:
			props->Equalizer.Mid2Width = val;
			break;

		case AL_EQUALIZER_MID3_GAIN:
			props->Equalizer.Mid3Gain = val;
			break;
		case AL_EQUALIZER_MID3_CENTER:
			props->Equalizer.Mid3Center = val;
			break;
		case AL_EQUALIZER_MID3_WIDTH:
			props->Equalizer.Mid3Width = val;
			break;

/******************new*******************/
		case AL_EQUALIZER_MID4_GAIN:
			props->Equalizer.Mid4Gain = val;
			break;
		case AL_EQUALIZER_MID4_CENTER:
			props->Equalizer.Mid4Center = val;
			break;
		case AL_EQUALIZER_MID4_WIDTH:
			props->Equalizer.Mid4Width = val;
			break;

		case AL_EQUALIZER_MID5_GAIN:
			props->Equalizer.Mid5Gain = val;
			break;
		case AL_EQUALIZER_MID5_CENTER:
			props->Equalizer.Mid5Center = val;
			break;
		case AL_EQUALIZER_MID5_WIDTH:
			props->Equalizer.Mid5Width = val;
			break;

		case AL_EQUALIZER_MID6_GAIN:
			props->Equalizer.Mid6Gain = val;
			break;
		case AL_EQUALIZER_MID6_CENTER:
			props->Equalizer.Mid6Center = val;
			break;
		case AL_EQUALIZER_MID6_WIDTH:
			props->Equalizer.Mid6Width = val;
			break;

		case AL_EQUALIZER_MID7_GAIN:
			props->Equalizer.Mid7Gain = val;
			break;
		case AL_EQUALIZER_MID7_CENTER:
			props->Equalizer.Mid7Center = val;
			break;
		case AL_EQUALIZER_MID7_WIDTH:
			props->Equalizer.Mid7Width = val;
			break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
void ALequalizer_setParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{
    ALequalizer_setParamf(effect, context, param, vals[0]);
}

void ALequalizer_getParami(const ALeffect *UNUSED(effect), ALCcontext *context, ALenum UNUSED(param), ALint *UNUSED(val))
{ SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM); }
void ALequalizer_getParamiv(const ALeffect *effect, ALCcontext *context, ALenum param, ALint *vals)
{
    ALequalizer_getParami(effect, context, param, vals);
}
void ALequalizer_getParamf(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{
    const ALeffectProps *props = &effect->Props;
    switch(param)
    {
        /*case AL_EQUALIZER_LOW_GAIN:
            *val = props->Equalizer.LowGain;
            break;

        case AL_EQUALIZER_LOW_CUTOFF:
            *val = props->Equalizer.LowCutoff;
            break;

        case AL_EQUALIZER_MID1_GAIN:
            *val = props->Equalizer.Mid1Gain;
            break;

        case AL_EQUALIZER_MID1_CENTER:
            *val = props->Equalizer.Mid1Center;
            break;

        case AL_EQUALIZER_MID1_WIDTH:
            *val = props->Equalizer.Mid1Width;
            break;

        case AL_EQUALIZER_MID2_GAIN:
            *val = props->Equalizer.Mid2Gain;
            break;

        case AL_EQUALIZER_MID2_CENTER:
            *val = props->Equalizer.Mid2Center;
            break;

        case AL_EQUALIZER_MID2_WIDTH:
            *val = props->Equalizer.Mid2Width;
            break;

        case AL_EQUALIZER_HIGH_GAIN:
            *val = props->Equalizer.HighGain;
            break;

        case AL_EQUALIZER_HIGH_CUTOFF:
            *val = props->Equalizer.HighCutoff;
            break;*/
		case AL_EQUALIZER_MID0_GAIN:
			*val = props->Equalizer.Mid0Gain;
			break;
		case AL_EQUALIZER_MID0_CENTER:
			*val = props->Equalizer.Mid0Center;
			break;
		case AL_EQUALIZER_MID0_WIDTH:
			*val = props->Equalizer.Mid0Width;
			break;

		case AL_EQUALIZER_MID1_GAIN:
			*val = props->Equalizer.Mid1Gain;
			break;
		case AL_EQUALIZER_MID1_CENTER:
			*val = props->Equalizer.Mid1Center;
			break;
		case AL_EQUALIZER_MID1_WIDTH:
			*val = props->Equalizer.Mid1Width;
			break;

		case AL_EQUALIZER_MID2_GAIN:
			*val = props->Equalizer.Mid2Gain;
			break;
		case AL_EQUALIZER_MID2_CENTER:
			*val = props->Equalizer.Mid2Center;
			break;
		case AL_EQUALIZER_MID2_WIDTH:
			*val = props->Equalizer.Mid2Width;
			break;

		case AL_EQUALIZER_MID3_GAIN:
			*val = props->Equalizer.Mid3Gain;
			break;
		case AL_EQUALIZER_MID3_CENTER:
			*val = props->Equalizer.Mid3Center;
			break;
		case AL_EQUALIZER_MID3_WIDTH:
			*val = props->Equalizer.Mid3Width;
			break;

	/******************new*******************/
		case AL_EQUALIZER_MID4_GAIN:
			*val = props->Equalizer.Mid4Gain;
			break;
		case AL_EQUALIZER_MID4_CENTER:
			*val = props->Equalizer.Mid4Center;
			break;
		case AL_EQUALIZER_MID4_WIDTH:
			*val = props->Equalizer.Mid4Width;
			break;

		case AL_EQUALIZER_MID5_GAIN:
			*val = props->Equalizer.Mid5Gain;
			break;
		case AL_EQUALIZER_MID5_CENTER:
			*val = props->Equalizer.Mid5Center;
			break;
		case AL_EQUALIZER_MID5_WIDTH:
			*val = props->Equalizer.Mid5Width;
			break;

		case AL_EQUALIZER_MID6_GAIN:
			*val = props->Equalizer.Mid6Gain;
			break;
		case AL_EQUALIZER_MID6_CENTER:
			*val = props->Equalizer.Mid6Center;
			break;
		case AL_EQUALIZER_MID6_WIDTH:
			*val = props->Equalizer.Mid6Width;
			break;

		case AL_EQUALIZER_MID7_GAIN:
			*val = props->Equalizer.Mid7Gain;
			break;
		case AL_EQUALIZER_MID7_CENTER:
			*val = props->Equalizer.Mid7Center;
			break;
		case AL_EQUALIZER_MID7_WIDTH:
			*val = props->Equalizer.Mid7Width;
			break;

        default:
            SET_ERROR_AND_RETURN(context, AL_INVALID_ENUM);
    }
}
void ALequalizer_getParamfv(const ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{
    ALequalizer_getParamf(effect, context, param, vals);
}

DEFINE_ALEFFECT_VTABLE(ALequalizer);
