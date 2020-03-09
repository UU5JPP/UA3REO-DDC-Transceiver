#include "audio_processor.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "arm_math.h"
#include "fpga.h"
#include "trx_manager.h"
#include "wm8731.h"
#include "functions.h"
#include "audio_filters.h"
#include "agc.h"
#include "settings.h"
#include "profiler.h"
#include "usbd_audio_if.h"
#include "noise_reduction.h"
#include "cw_decoder.h"

volatile uint32_t AUDIOPROC_samples = 0;
volatile uint32_t AUDIOPROC_TXA_samples = 0;
volatile uint32_t AUDIOPROC_TXB_samples = 0;
SRAM1 int32_t Processor_AudioBuffer_A[FPGA_AUDIO_BUFFER_SIZE] = {0};
SRAM1 int32_t Processor_AudioBuffer_B[FPGA_AUDIO_BUFFER_SIZE] = {0};
volatile uint_fast8_t Processor_AudioBuffer_ReadyBuffer = 0;
volatile bool Processor_NeedRXBuffer = false;
volatile bool Processor_NeedTXBuffer = false;
SRAM1 float32_t FPGA_Audio_Buffer_RX1_Q_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
SRAM1 float32_t FPGA_Audio_Buffer_RX1_I_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
SRAM1 float32_t FPGA_Audio_Buffer_RX2_Q_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
SRAM1 float32_t FPGA_Audio_Buffer_RX2_I_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
SRAM1 float32_t FPGA_Audio_Buffer_TX_Q_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
SRAM1 float32_t FPGA_Audio_Buffer_TX_I_tmp[FPGA_AUDIO_BUFFER_HALF_SIZE] = {0};
volatile float32_t Processor_AVG_amplitude = 0.0f;		  //средняя амплитуда семплов при прёме
volatile float32_t Processor_TX_MAX_amplitude_IN = 0.0f;  //средняя амплитуда семплов при передаче (до процессора)
volatile float32_t Processor_TX_MAX_amplitude_OUT = 0.0f; //средняя амплитуда семплов при передаче (после процессора)
volatile float32_t Processor_RX_Power_value = 0.0f;		  //максимальное значение силы сигнала
volatile float32_t ALC_need_gain = 1.0f;
volatile float32_t Processor_selected_RFpower_amplitude = 0.0f;

static float32_t ampl_val_i = 0.0f;
static float32_t ampl_val_q = 0.0f;
static float32_t ALC_need_gain_target = 1.0f;
static uint_fast16_t block = 0;
static uint32_t two_signal_gen_position = 0;

static void doRX_HILBERT(AUDIO_PROC_RX_NUM rx_id);
static void doRX_LPF(AUDIO_PROC_RX_NUM rx_id);
static void doRX_HPF(AUDIO_PROC_RX_NUM rx_id);
static void doRX_DNR(AUDIO_PROC_RX_NUM rx_id);
static void doRX_AGC(AUDIO_PROC_RX_NUM rx_id);
static void doRX_NOTCH(AUDIO_PROC_RX_NUM rx_id);
static void doRX_SMETER(AUDIO_PROC_RX_NUM rx_id);
static void doRX_COPYCHANNEL(AUDIO_PROC_RX_NUM rx_id);
static void doCW_Decode(AUDIO_PROC_RX_NUM rx_id);
static void DemodulateFM(AUDIO_PROC_RX_NUM rx_id);
static void ModulateFM(void);

void initAudioProcessor(void)
{
	InitAudioFilters();
	InitAGC();
	sendToDebug_strln("[OK] Audioprocessor inited");
}

void processRxAudio(void)
{
	if (!Processor_NeedRXBuffer)
		return;
	VFO *current_vfo = CurrentVFO();
	VFO *secondary_vfo = SecondaryVFO();

	AUDIOPROC_samples++;
	uint_fast16_t FPGA_Audio_Buffer_Index_tmp = FPGA_Audio_Buffer_Index;
	if (FPGA_Audio_Buffer_Index_tmp == 0)
		FPGA_Audio_Buffer_Index_tmp = FPGA_AUDIO_BUFFER_SIZE;
	else
		FPGA_Audio_Buffer_Index_tmp--;

	readHalfFromCircleBuffer32((uint32_t *)&FPGA_Audio_Buffer_RX1_Q[0], (uint32_t *)&FPGA_Audio_Buffer_RX1_Q_tmp[0], FPGA_Audio_Buffer_Index_tmp, FPGA_AUDIO_BUFFER_SIZE);
	readHalfFromCircleBuffer32((uint32_t *)&FPGA_Audio_Buffer_RX1_I[0], (uint32_t *)&FPGA_Audio_Buffer_RX1_I_tmp[0], FPGA_Audio_Buffer_Index_tmp, FPGA_AUDIO_BUFFER_SIZE);

	if(TRX.Dual_RX_Type != VFO_SEPARATE)
	{
		readHalfFromCircleBuffer32((uint32_t *)&FPGA_Audio_Buffer_RX2_Q[0], (uint32_t *)&FPGA_Audio_Buffer_RX2_Q_tmp[0], FPGA_Audio_Buffer_Index_tmp, FPGA_AUDIO_BUFFER_SIZE);
		readHalfFromCircleBuffer32((uint32_t *)&FPGA_Audio_Buffer_RX2_I[0], (uint32_t *)&FPGA_Audio_Buffer_RX2_I_tmp[0], FPGA_Audio_Buffer_Index_tmp, FPGA_AUDIO_BUFFER_SIZE);
	}
	
	//Process DC corrector filter
	if(current_vfo->Mode != TRX_MODE_AM && current_vfo->Mode != TRX_MODE_NFM && current_vfo->Mode != TRX_MODE_WFM)
	{
		dc_filter(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_RX1_I);
		dc_filter(FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_RX1_Q);
	}
	if(TRX.Dual_RX_Type != VFO_SEPARATE && secondary_vfo->Mode != TRX_MODE_AM && secondary_vfo->Mode != TRX_MODE_NFM && secondary_vfo->Mode != TRX_MODE_WFM)
	{
		dc_filter(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_RX2_I);
		dc_filter(FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_RX2_Q);
	}

	//Применяем усиление ПЧ IF Gain
	if(current_vfo->Mode != TRX_MODE_IQ)
	{
		arm_scale_f32(FPGA_Audio_Buffer_RX1_I_tmp, db2rateV(TRX.IF_Gain), FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_scale_f32(FPGA_Audio_Buffer_RX1_Q_tmp, db2rateV(TRX.IF_Gain), FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		if(TRX.Dual_RX_Type != VFO_SEPARATE)
		{
			arm_scale_f32(FPGA_Audio_Buffer_RX2_I_tmp, db2rateV(TRX.IF_Gain), FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_scale_f32(FPGA_Audio_Buffer_RX2_Q_tmp, db2rateV(TRX.IF_Gain), FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		}
	}

	switch (current_vfo->Mode) //первый приёмник
	{
	case TRX_MODE_LSB:
	case TRX_MODE_CW_L:
		doRX_HPF(AUDIO_RX1);
	case TRX_MODE_DIGI_L:
		doRX_HILBERT(AUDIO_RX1);
		doRX_LPF(AUDIO_RX1);
		arm_sub_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE); // difference of I and Q - LSB
		doRX_NOTCH(AUDIO_RX1);
		doRX_SMETER(AUDIO_RX1);
		doRX_DNR(AUDIO_RX1);
		doRX_AGC(AUDIO_RX1);
		doCW_Decode(AUDIO_RX1);
		doRX_COPYCHANNEL(AUDIO_RX1);
		break;
	case TRX_MODE_USB:
	case TRX_MODE_CW_U:
		doRX_HPF(AUDIO_RX1);
	case TRX_MODE_DIGI_U:
		doRX_HILBERT(AUDIO_RX1);
		doRX_LPF(AUDIO_RX1);
		arm_add_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE); // sum of I and Q - USB
		doRX_NOTCH(AUDIO_RX1);
		doRX_SMETER(AUDIO_RX1);
		doRX_DNR(AUDIO_RX1);
		doRX_AGC(AUDIO_RX1);
		doCW_Decode(AUDIO_RX1);
		doRX_COPYCHANNEL(AUDIO_RX1);
		break;
	case TRX_MODE_AM:
		doRX_LPF(AUDIO_RX1);
		arm_mult_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_mult_f32(FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_add_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
			arm_sqrt_f32(FPGA_Audio_Buffer_RX1_I_tmp[i], &FPGA_Audio_Buffer_RX1_I_tmp[i]);
		doRX_NOTCH(AUDIO_RX1);
		doRX_SMETER(AUDIO_RX1);
		doRX_DNR(AUDIO_RX1);
		doRX_AGC(AUDIO_RX1);
		doRX_COPYCHANNEL(AUDIO_RX1);
		break;
	case TRX_MODE_NFM:
	case TRX_MODE_WFM:
		doRX_LPF(AUDIO_RX1);
		DemodulateFM(AUDIO_RX1);
		doRX_SMETER(AUDIO_RX1);
		doRX_DNR(AUDIO_RX1);
		doRX_AGC(AUDIO_RX1);
		doRX_COPYCHANNEL(AUDIO_RX1);
		break;
	case TRX_MODE_IQ:
	default:
		doRX_SMETER(AUDIO_RX1);
		break;
	}
	
	if(TRX.Dual_RX_Type != VFO_SEPARATE)
	{
		switch (secondary_vfo->Mode) //второй приёмник
		{
		case TRX_MODE_LSB:
		case TRX_MODE_CW_L:
			doRX_HPF(AUDIO_RX2);
		case TRX_MODE_DIGI_L:
			doRX_HILBERT(AUDIO_RX2);
			doRX_LPF(AUDIO_RX2);
			arm_sub_f32(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE); // difference of I and Q - LSB
			doRX_NOTCH(AUDIO_RX2);
			doRX_DNR(AUDIO_RX2);
			doRX_AGC(AUDIO_RX2);
			break;
		case TRX_MODE_USB:
		case TRX_MODE_CW_U:
			doRX_HPF(AUDIO_RX2);
		case TRX_MODE_DIGI_U:
			doRX_HILBERT(AUDIO_RX2);
			doRX_LPF(AUDIO_RX2);
			arm_add_f32(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE); // sum of I and Q - USB
			doRX_NOTCH(AUDIO_RX2);
			doRX_DNR(AUDIO_RX2);
			doRX_AGC(AUDIO_RX2);
			break;
		case TRX_MODE_AM:
			doRX_LPF(AUDIO_RX2);
			arm_mult_f32(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_mult_f32(FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_add_f32(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
				arm_sqrt_f32(FPGA_Audio_Buffer_RX2_I_tmp[i], &FPGA_Audio_Buffer_RX2_I_tmp[i]);
			doRX_NOTCH(AUDIO_RX2);
			doRX_DNR(AUDIO_RX2);
			doRX_AGC(AUDIO_RX2);
			break;
		case TRX_MODE_NFM:
		case TRX_MODE_WFM:
			doRX_LPF(AUDIO_RX2);
			DemodulateFM(AUDIO_RX2);
			doRX_DNR(AUDIO_RX2);
			doRX_AGC(AUDIO_RX2);
			break;
		case TRX_MODE_IQ:
		default:
			break;
		}
	}

	//Prepare data to DMA
	int32_t* Processor_AudioBuffer_current;
	if (Processor_AudioBuffer_ReadyBuffer == 0)
		Processor_AudioBuffer_current = &Processor_AudioBuffer_B[0];
	else
		Processor_AudioBuffer_current = &Processor_AudioBuffer_A[0];
	
	if(TRX.Dual_RX_Type == VFO_A_PLUS_B)
	{
		arm_add_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_scale_f32(FPGA_Audio_Buffer_RX1_I_tmp, 0.5f, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}
	
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		if(TRX.Dual_RX_Type == VFO_SEPARATE)
		{
			arm_float_to_q31(&FPGA_Audio_Buffer_RX1_I_tmp[i], &Processor_AudioBuffer_current[i * 2], 1); //left channel
			arm_float_to_q31(&FPGA_Audio_Buffer_RX1_Q_tmp[i], &Processor_AudioBuffer_current[i * 2 + 1], 1); //right channel
		}
		else if(TRX.Dual_RX_Type == VFO_A_AND_B)
		{
			if(!TRX.current_vfo)
			{
				arm_float_to_q31(&FPGA_Audio_Buffer_RX1_I_tmp[i], &Processor_AudioBuffer_current[i * 2], 1); //left channel
				arm_float_to_q31(&FPGA_Audio_Buffer_RX2_I_tmp[i], &Processor_AudioBuffer_current[i * 2 + 1], 1); //right channel
			}
			else
			{
				arm_float_to_q31(&FPGA_Audio_Buffer_RX2_I_tmp[i], &Processor_AudioBuffer_current[i * 2], 1); //left channel
				arm_float_to_q31(&FPGA_Audio_Buffer_RX1_I_tmp[i], &Processor_AudioBuffer_current[i * 2 + 1], 1); //right channel
			}
		}
		else if(TRX.Dual_RX_Type == VFO_A_PLUS_B)
		{
			arm_float_to_q31(&FPGA_Audio_Buffer_RX1_I_tmp[i], &Processor_AudioBuffer_current[i * 2], 1); //left channel
			Processor_AudioBuffer_current[i * 2 + 1] = Processor_AudioBuffer_current[i * 2]; //right channel
		}
	}
	if (Processor_AudioBuffer_ReadyBuffer == 0)
		Processor_AudioBuffer_ReadyBuffer = 1;
	else
		Processor_AudioBuffer_ReadyBuffer = 0;

	//Send to USB Audio
	if (USB_AUDIO_need_rx_buffer && TRX_Inited)
	{
		uint8_t* USB_AUDIO_rx_buffer_current;
			
		if (!USB_AUDIO_current_rx_buffer)
			USB_AUDIO_rx_buffer_current = &USB_AUDIO_rx_buffer_a[0];
		else
			USB_AUDIO_rx_buffer_current = &USB_AUDIO_rx_buffer_b[0];
			
		//drop LSB 32b->24b
		for (uint_fast16_t i = 0; i < (USB_AUDIO_RX_BUFFER_SIZE / BYTES_IN_SAMPLE_AUDIO_OUT_PACKET); i++)
		{
			USB_AUDIO_rx_buffer_current[i*BYTES_IN_SAMPLE_AUDIO_OUT_PACKET+0] = (Processor_AudioBuffer_current[i] >> 8) & 0xFF;
			USB_AUDIO_rx_buffer_current[i*BYTES_IN_SAMPLE_AUDIO_OUT_PACKET+1] = (Processor_AudioBuffer_current[i] >> 16) & 0xFF;
			USB_AUDIO_rx_buffer_current[i*BYTES_IN_SAMPLE_AUDIO_OUT_PACKET+2] = (Processor_AudioBuffer_current[i] >> 24) & 0xFF;
		}
		USB_AUDIO_need_rx_buffer = false;
	}
	//

	//OUT Volume
	float32_t volume_gain = volume2rate((float32_t)TRX_Volume / 1023.0f);
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_SIZE; i++)
	{
		Processor_AudioBuffer_current[i] = Processor_AudioBuffer_current[i] * volume_gain;
		Processor_AudioBuffer_current[i] = convertToSPIBigEndian(Processor_AudioBuffer_current[i]); //for 32bit audio
	}

	//Send to Codec DMA
	if (TRX_Inited)
	{
		if (WM8731_DMA_state) //complete
		{
			HAL_DMA_Start_IT(&hdma_memtomem_dma2_stream0, (uint32_t)&Processor_AudioBuffer_current[0], (uint32_t)&CODEC_Audio_Buffer_RX[FPGA_AUDIO_BUFFER_SIZE], FPGA_AUDIO_BUFFER_SIZE);
			AUDIOPROC_TXA_samples++;
		}
		else //half
		{
			HAL_DMA_Start_IT(&hdma_memtomem_dma2_stream1, (uint32_t)&Processor_AudioBuffer_current[0], (uint32_t)&CODEC_Audio_Buffer_RX[0], FPGA_AUDIO_BUFFER_SIZE);
			AUDIOPROC_TXB_samples++;
		}
	}

	Processor_NeedRXBuffer = false;
}

void processTxAudio(void)
{
	if (!Processor_NeedTXBuffer)
		return;
	VFO *current_vfo = CurrentVFO();
	AUDIOPROC_samples++;
	Processor_selected_RFpower_amplitude = ((log10f_fast((float32_t)TRX.RF_Power / 10) + 1) / 2.0f) * TRX_MAX_TX_Amplitude;
	uint_fast8_t mode = current_vfo->Mode;
	if ((TRX_Tune && !TRX.TWO_SIGNAL_TUNE) || mode == TRX_MODE_CW_L || mode == TRX_MODE_CW_U)
		Processor_selected_RFpower_amplitude = Processor_selected_RFpower_amplitude * 0.7f; // поправка на нулевые биения

	if (TRX.InputType_USB) //USB AUDIO
	{
		uint_fast16_t buffer_index = USB_AUDIO_GetTXBufferIndex_FS() /BYTES_IN_SAMPLE_AUDIO_OUT_PACKET; //buffer 8bit, data 24 bit
		if ((buffer_index % BYTES_IN_SAMPLE_AUDIO_OUT_PACKET) == 1)
			buffer_index-=(buffer_index % BYTES_IN_SAMPLE_AUDIO_OUT_PACKET);
		readHalfFromCircleUSBBuffer24Bit(&USB_AUDIO_tx_buffer[0], &Processor_AudioBuffer_A[0], buffer_index, (USB_AUDIO_TX_BUFFER_SIZE / BYTES_IN_SAMPLE_AUDIO_OUT_PACKET));
	}
	else //AUDIO CODEC AUDIO
	{
		uint_fast16_t dma_index = CODEC_AUDIO_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(hi2s3.hdmarx);
		if ((dma_index % 2) == 1) dma_index--;
		readHalfFromCircleBuffer32((uint32_t *)&CODEC_Audio_Buffer_TX[0], (uint32_t *)&Processor_AudioBuffer_A[0], dma_index, CODEC_AUDIO_BUFFER_SIZE);
		for(uint_fast16_t i; i < FPGA_AUDIO_BUFFER_HALF_SIZE ; i++)
			Processor_AudioBuffer_A[i] = convertToSPIBigEndian (Processor_AudioBuffer_A[i]);
	}

	//One-signal zero-tune generator
	if (TRX_Tune && !TRX.TWO_SIGNAL_TUNE)
	{
		for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			FPGA_Audio_Buffer_TX_I_tmp[i] = (Processor_selected_RFpower_amplitude / 100.0f * TUNE_POWER);
			FPGA_Audio_Buffer_TX_Q_tmp[i] = (Processor_selected_RFpower_amplitude / 100.0f * TUNE_POWER);
		}
	}

	//Two-signal tune generator
	if (TRX_Tune && TRX.TWO_SIGNAL_TUNE)
	{
		for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			float32_t point = generateSin((Processor_selected_RFpower_amplitude / 100.0f * TUNE_POWER) / 2.0f, two_signal_gen_position, TRX_SAMPLERATE, 1000);
			point += generateSin((Processor_selected_RFpower_amplitude / 100.0f * TUNE_POWER) / 2.0f, two_signal_gen_position, TRX_SAMPLERATE, 2000);
			two_signal_gen_position++;
			if (two_signal_gen_position > TRX_SAMPLERATE)
				two_signal_gen_position = 0;
			FPGA_Audio_Buffer_TX_I_tmp[i] = point;
			FPGA_Audio_Buffer_TX_Q_tmp[i] = point;
		}
		//hilbert fir
		// + 45 deg to Q data
		arm_fir_f32(&FIR_TX_Hilbert_Q, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		// - 45 deg to I data
		arm_fir_f32(&FIR_TX_Hilbert_I, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}

	//Copy and convert buffer
	if (!TRX_Tune)
	{
		for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			FPGA_Audio_Buffer_TX_I_tmp[i] = (float32_t)Processor_AudioBuffer_A[i * 2] / 2147483648.0f;
			FPGA_Audio_Buffer_TX_Q_tmp[i] = (float32_t)Processor_AudioBuffer_A[i * 2 + 1] / 2147483648.0f;
		}
	}

	//Process DC corrector filter
	if (!TRX_Tune)
	{
		dc_filter(FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_TX_I);
		dc_filter(FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, DC_FILTER_TX_Q);
	}

	if (mode != TRX_MODE_IQ && !TRX_Tune)
	{
		//IIR HPF
		if (current_vfo->HPF_Filter_Width > 0)
			arm_iir_lattice_f32(&IIR_TX_HPF_I, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		//IIR LPF
		if (current_vfo->LPF_Filter_Width > 0)
			arm_iir_lattice_f32(&IIR_TX_LPF_I, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		memcpy(&FPGA_Audio_Buffer_TX_Q_tmp[0], &FPGA_Audio_Buffer_TX_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE * 4); //double left and right channel

		switch (mode)
		{
		case TRX_MODE_CW_L:
		case TRX_MODE_CW_U:
			if (!TRX_key_serial && !TRX_ptt_hard && !TRX_key_dot_hard && !TRX_key_dash_hard)
				Processor_selected_RFpower_amplitude = 0;
			float32_t cw_signal = TRX_GenerateCWSignal(Processor_selected_RFpower_amplitude);
			for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
			{
				FPGA_Audio_Buffer_TX_Q_tmp[i] = cw_signal;
				FPGA_Audio_Buffer_TX_I_tmp[i] = cw_signal;
			}
			break;
		case TRX_MODE_USB:
		case TRX_MODE_DIGI_U:
			//hilbert fir
			// + 45 deg to Q data
			arm_fir_f32(&FIR_TX_Hilbert_Q, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			// - 45 deg to I data
			arm_fir_f32(&FIR_TX_Hilbert_I, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			break;
		case TRX_MODE_LSB:
		case TRX_MODE_DIGI_L:
			//hilbert fir
			// + 45 deg to I data
			arm_fir_f32(&FIR_TX_Hilbert_I, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			// - 45 deg to Q data
			arm_fir_f32(&FIR_TX_Hilbert_Q, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			break;
		case TRX_MODE_AM:
			// + 45 deg to I data
			arm_fir_f32(&FIR_TX_Hilbert_I, FPGA_Audio_Buffer_TX_I_tmp, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			// - 45 deg to Q data
			arm_fir_f32(&FIR_TX_Hilbert_Q, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			for (size_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
			{
				float32_t i_am = ((FPGA_Audio_Buffer_TX_I_tmp[i] - FPGA_Audio_Buffer_TX_Q_tmp[i]) + (Processor_selected_RFpower_amplitude)) / 2.0f;
				float32_t q_am = ((FPGA_Audio_Buffer_TX_Q_tmp[i] - FPGA_Audio_Buffer_TX_I_tmp[i]) - (Processor_selected_RFpower_amplitude)) / 2.0f;
				FPGA_Audio_Buffer_TX_I_tmp[i] = i_am;
				FPGA_Audio_Buffer_TX_Q_tmp[i] = q_am;
			}
			break;
		case TRX_MODE_NFM:
		case TRX_MODE_WFM:
			ModulateFM();
			break;
		default:
			break;
		}
	}

	//RF PowerControl (Audio Level Control) Compressor
	Processor_TX_MAX_amplitude_IN = 0;
	//ищем максимум в амплитуде
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		arm_abs_f32(&FPGA_Audio_Buffer_TX_I_tmp[i], &ampl_val_i, 1);
		arm_abs_f32(&FPGA_Audio_Buffer_TX_Q_tmp[i], &ampl_val_q, 1);
		if (ampl_val_i > Processor_TX_MAX_amplitude_IN)
			Processor_TX_MAX_amplitude_IN = ampl_val_i;
		if (ampl_val_q > Processor_TX_MAX_amplitude_IN)
			Processor_TX_MAX_amplitude_IN = ampl_val_q;
	}
	//расчитываем целевое значение усиления
	if (Processor_TX_MAX_amplitude_IN > 0.0f)
	{
		ALC_need_gain_target = Processor_selected_RFpower_amplitude / Processor_TX_MAX_amplitude_IN;
		//двигаем усиление на шаг
		if (ALC_need_gain_target > ALC_need_gain)
			ALC_need_gain += (ALC_need_gain_target - ALC_need_gain) / (100.0f / (float32_t)TRX.TX_AGC_speed);
		else
			ALC_need_gain -= (ALC_need_gain - ALC_need_gain_target) / (100.0f / (float32_t)TRX.TX_AGC_speed);

		if (ALC_need_gain_target < ALC_need_gain)
			ALC_need_gain = ALC_need_gain_target;
		if (ALC_need_gain < 0.0f)
			ALC_need_gain = 0.0f;
		//перегрузка (клиппинг), резко снижаем усиление
		if ((ALC_need_gain * Processor_TX_MAX_amplitude_IN) > (Processor_selected_RFpower_amplitude * 1.1f))
			ALC_need_gain = ALC_need_gain_target;
		if (ALC_need_gain > TX_AGC_MAXGAIN)
			ALC_need_gain = TX_AGC_MAXGAIN;
		//шумовой порог
		if (Processor_TX_MAX_amplitude_IN < TX_AGC_NOISEGATE)
			ALC_need_gain = 0.0f;
	}
	//оключаем усиление для некоторых видов мод
	if ((ALC_need_gain > 1.0f) && (mode == TRX_MODE_LOOPBACK))
		ALC_need_gain = 1.0f;
	if (mode == TRX_MODE_CW_L || mode == TRX_MODE_CW_U || mode == TRX_MODE_NFM || mode == TRX_MODE_WFM)
		ALC_need_gain = 1.0f;
	if (TRX_Tune)
		ALC_need_gain = 1.0f;

	//применяем усиление
	arm_scale_f32(FPGA_Audio_Buffer_TX_I_tmp, ALC_need_gain, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	arm_scale_f32(FPGA_Audio_Buffer_TX_Q_tmp, ALC_need_gain, FPGA_Audio_Buffer_TX_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	//

	Processor_TX_MAX_amplitude_OUT = Processor_TX_MAX_amplitude_IN * ALC_need_gain;
	//RF PowerControl (Audio Level Control) Compressor END

	//Send TX data to FFT
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		if (NeedFFTInputBuffer)
		{
			FFTInput_I[FFT_buff_index] = FPGA_Audio_Buffer_TX_I_tmp[i];
			FFTInput_Q[FFT_buff_index] = FPGA_Audio_Buffer_TX_Q_tmp[i];
			FFT_buff_index++;
			if (FFT_buff_index >= FFT_SIZE)
			{
				FFT_buff_index = 0;
				NeedFFTInputBuffer = false;
				FFT_buffer_ready = true;
			}
		}
	}

	//Loopback mode
	if (mode == TRX_MODE_LOOPBACK && !TRX_Tune)
	{
		//OUT Volume
		doRX_AGC(AUDIO_RX1);
		float32_t volume_gain = volume2rate((float32_t)TRX_Volume / 1023.0f);
		arm_scale_f32(FPGA_Audio_Buffer_TX_I_tmp, volume_gain, FPGA_Audio_Buffer_TX_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);

		for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
		{
			arm_float_to_q31(&FPGA_Audio_Buffer_TX_I_tmp[i], &Processor_AudioBuffer_A[i * 2], 1);
			Processor_AudioBuffer_A[i * 2] = convertToSPIBigEndian(Processor_AudioBuffer_A[i * 2]);		 //left channel
			Processor_AudioBuffer_A[i * 2 + 1] = Processor_AudioBuffer_A[i * 2]; //right channel
		}

		if (WM8731_DMA_state) //compleate
		{
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[FPGA_AUDIO_BUFFER_SIZE], FPGA_AUDIO_BUFFER_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			AUDIOPROC_TXA_samples++;
		}
		else //half
		{
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&Processor_AudioBuffer_A[0], (uint32_t)&CODEC_Audio_Buffer_RX[0], FPGA_AUDIO_BUFFER_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			AUDIOPROC_TXB_samples++;
		}
	}
	else
	{
		//CW SelfHear
		if (TRX.CW_SelfHear && (TRX_key_serial || TRX_key_dot_hard || TRX_key_dash_hard) && (mode == TRX_MODE_CW_L || mode == TRX_MODE_CW_U) && !TRX_Tune)
		{
			if (Processor_TX_MAX_amplitude_IN > 0)
			{
				for (uint_fast16_t i = 0; i < CODEC_AUDIO_BUFFER_SIZE; i++)
					CODEC_Audio_Buffer_RX[i] = convertToSPIBigEndian(((float32_t)TRX_Volume / 100.0f) * 2000.0f * arm_sin_f32(((float32_t)i / (float32_t)TRX_SAMPLERATE) * PI * 2.0f * (float32_t)TRX.CW_GENERATOR_SHIFT_HZ));
			}
			else
			{
				memset(CODEC_Audio_Buffer_RX, 0x00, sizeof CODEC_Audio_Buffer_RX);
			}
		}
		//
		if (FPGA_Audio_Buffer_State) //Send to FPGA DMA
		{
			AUDIOPROC_TXA_samples++;
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&FPGA_Audio_Buffer_TX_I_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_I[FPGA_AUDIO_BUFFER_HALF_SIZE], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			HAL_DMA_Start(&hdma_memtomem_dma2_stream0, (uint32_t)&FPGA_Audio_Buffer_TX_Q_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_Q[FPGA_AUDIO_BUFFER_HALF_SIZE], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream0, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		}
		else
		{
			AUDIOPROC_TXB_samples++;
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&FPGA_Audio_Buffer_TX_I_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_I[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
			HAL_DMA_Start(&hdma_memtomem_dma2_stream1, (uint32_t)&FPGA_Audio_Buffer_TX_Q_tmp[0], (uint32_t)&FPGA_Audio_SendBuffer_Q[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
			HAL_DMA_PollForTransfer(&hdma_memtomem_dma2_stream1, HAL_DMA_FULL_TRANSFER, HAL_MAX_DELAY);
		}
	}

	Processor_NeedTXBuffer = false;
	Processor_NeedRXBuffer = false;
}

static void doCW_Decode(AUDIO_PROC_RX_NUM rx_id)
{
	//CW Decoder
	if(rx_id==AUDIO_RX1)
	{
		if (TRX.CWDecoder && (CurrentVFO()->Mode == TRX_MODE_CW_L || CurrentVFO()->Mode == TRX_MODE_CW_U))
			for (block = 0; block < (FPGA_AUDIO_BUFFER_HALF_SIZE / CWDECODER_SAMPLES); block++)
				CWDecoder_Process(FPGA_Audio_Buffer_RX1_I_tmp + (block * CWDECODER_SAMPLES));
	}
	else if(rx_id==AUDIO_RX2)
	{
		if (TRX.CWDecoder && (CurrentVFO()->Mode == TRX_MODE_CW_L || CurrentVFO()->Mode == TRX_MODE_CW_U))
			for (block = 0; block < (FPGA_AUDIO_BUFFER_HALF_SIZE / CWDECODER_SAMPLES); block++)
				CWDecoder_Process(FPGA_Audio_Buffer_RX2_I_tmp + (block * CWDECODER_SAMPLES));
	}
}

static void doRX_HILBERT(AUDIO_PROC_RX_NUM rx_id)
{
	//Hilbert fir
	if(rx_id==AUDIO_RX1)
	{
		arm_fir_f32(&FIR_RX1_Hilbert_I, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_fir_f32(&FIR_RX1_Hilbert_Q, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}
	else if(rx_id==AUDIO_RX2)
	{
		arm_fir_f32(&FIR_RX2_Hilbert_I, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		arm_fir_f32(&FIR_RX2_Hilbert_Q, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}
}

static void doRX_LPF(AUDIO_PROC_RX_NUM rx_id)
{
	//IIR LPF
	if(rx_id==AUDIO_RX1)
	{
		if (CurrentVFO()->LPF_Filter_Width > 0)
		{
			arm_iir_lattice_f32(&IIR_RX1_LPF_I, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_iir_lattice_f32(&IIR_RX1_LPF_Q, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		}
	}
	else if(rx_id==AUDIO_RX2)
	{
		if (SecondaryVFO()->LPF_Filter_Width > 0)
		{
			arm_iir_lattice_f32(&IIR_RX2_LPF_I, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_iir_lattice_f32(&IIR_RX2_LPF_Q, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		}
	}
}

static void doRX_HPF(AUDIO_PROC_RX_NUM rx_id)
{
	//IIR HPF
	if(rx_id==AUDIO_RX1)
	{
		if (CurrentVFO()->HPF_Filter_Width > 0)
		{
			arm_iir_lattice_f32(&IIR_RX1_HPF_I, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_iir_lattice_f32(&IIR_RX1_HPF_Q, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_Audio_Buffer_RX1_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		}
	}
	else if(rx_id==AUDIO_RX2)
	{
		if (SecondaryVFO()->HPF_Filter_Width > 0)
		{
			arm_iir_lattice_f32(&IIR_RX2_HPF_I, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
			arm_iir_lattice_f32(&IIR_RX2_HPF_Q, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_Audio_Buffer_RX2_Q_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
		}
	}
}

static void doRX_NOTCH(AUDIO_PROC_RX_NUM rx_id)
{
	//NOTCH FILTER
	if(rx_id==AUDIO_RX1)
	{
		if (CurrentVFO()->NotchFilter)
			arm_biquad_cascade_df2T_f32(&NOTCH_RX1_FILTER, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}
	else if(rx_id==AUDIO_RX2)
	{
		if (SecondaryVFO()->NotchFilter)
			arm_biquad_cascade_df2T_f32(&NOTCH_RX2_FILTER, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE);
	}
}

static void doRX_DNR(AUDIO_PROC_RX_NUM rx_id)
{
	//Digital Noise Reduction
	if(rx_id==AUDIO_RX1)
	{
		if (CurrentVFO()->DNR > 0)
		{
			for (block = 0; block < (FPGA_AUDIO_BUFFER_HALF_SIZE / NOISE_REDUCTION_BLOCK_SIZE); block++)
				processNoiseReduction(FPGA_Audio_Buffer_RX1_I_tmp + (block * NOISE_REDUCTION_BLOCK_SIZE), FPGA_Audio_Buffer_RX1_I_tmp + (block * NOISE_REDUCTION_BLOCK_SIZE), rx_id);
		}
	}
	else if(rx_id==AUDIO_RX2)
	{
		if (SecondaryVFO()->DNR > 0)
		{
			for (block = 0; block < (FPGA_AUDIO_BUFFER_HALF_SIZE / NOISE_REDUCTION_BLOCK_SIZE); block++)
				processNoiseReduction(FPGA_Audio_Buffer_RX2_I_tmp + (block * NOISE_REDUCTION_BLOCK_SIZE), FPGA_Audio_Buffer_RX2_I_tmp + (block * NOISE_REDUCTION_BLOCK_SIZE), rx_id);
		}
	}
}

static void doRX_AGC(AUDIO_PROC_RX_NUM rx_id)
{
	//AGC
	if(rx_id==AUDIO_RX1)
		DoAGC(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, rx_id);
	else if(rx_id==AUDIO_RX2)
		DoAGC(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, rx_id);
}

static void doRX_SMETER(AUDIO_PROC_RX_NUM rx_id)
{
	//Prepare data to calculate s-meter
	float32_t i = 0;
	if(rx_id==AUDIO_RX1)
		arm_power_f32(FPGA_Audio_Buffer_RX1_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, &i);
	else if(rx_id==AUDIO_RX2)
		arm_power_f32(FPGA_Audio_Buffer_RX2_I_tmp, FPGA_AUDIO_BUFFER_HALF_SIZE, &i);
	Processor_RX_Power_value = i / (float32_t)FPGA_AUDIO_BUFFER_HALF_SIZE;
}

static void doRX_COPYCHANNEL(AUDIO_PROC_RX_NUM rx_id)
{
	//Double channel I->Q
	if(rx_id==AUDIO_RX1)
		dma_memcpy32((uint32_t)&FPGA_Audio_Buffer_RX1_Q_tmp[0], (uint32_t)&FPGA_Audio_Buffer_RX1_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
	else if(rx_id==AUDIO_RX2)
		dma_memcpy32((uint32_t)&FPGA_Audio_Buffer_RX2_Q_tmp[0], (uint32_t)&FPGA_Audio_Buffer_RX2_I_tmp[0], FPGA_AUDIO_BUFFER_HALF_SIZE);
}

static float32_t DFM_RX1_lpf_prev = 0, DFM_RX1_hpf_prev_a = 0, DFM_RX1_hpf_prev_b = 0, DFM_RX2_lpf_prev = 0, DFM_RX2_hpf_prev_a = 0, DFM_RX2_hpf_prev_b = 0; // used in FM detection and low/high pass processing
static float32_t DFM_RX1_i_prev = 0, DFM_RX1_q_prev = 0, DFM_RX2_i_prev = 0, DFM_RX2_q_prev = 0; // used in FM detection and low/high pass processing
static uint_fast8_t DFM_RX1_fm_sql_count = 0, DFM_RX2_fm_sql_count = 0; // used for squelch processing and debouncing tone detection, respectively
float32_t DFM_RX1_fm_sql_avg = 0.0f;
float32_t DFM_RX2_fm_sql_avg = 0.0f;

static void DemodulateFM(AUDIO_PROC_RX_NUM rx_id)
{
	float32_t angle, x, y, a, b;
	float32_t squelch_buf[FPGA_AUDIO_BUFFER_HALF_SIZE];
	
	float32_t* lpf_prev;
	float32_t* hpf_prev_a;
	float32_t* hpf_prev_b;
	float32_t* i_prev;
	float32_t* q_prev;
	uint_fast8_t* fm_sql_count;
	float32_t* FPGA_Audio_Buffer_I_tmp;
	float32_t* FPGA_Audio_Buffer_Q_tmp;
	float32_t* fm_sql_avg;
	if(rx_id==AUDIO_RX1)
	{
		lpf_prev = &DFM_RX1_lpf_prev;
		hpf_prev_a = &DFM_RX1_hpf_prev_a;
		hpf_prev_b = &DFM_RX1_hpf_prev_b;
		i_prev = &DFM_RX1_i_prev;
		q_prev = &DFM_RX1_q_prev;
		fm_sql_count = &DFM_RX1_fm_sql_count;
		fm_sql_avg = &DFM_RX1_fm_sql_avg;
		FPGA_Audio_Buffer_I_tmp = &FPGA_Audio_Buffer_RX1_I_tmp[0];
		FPGA_Audio_Buffer_Q_tmp = &FPGA_Audio_Buffer_RX1_Q_tmp[0];
	}
	else if(rx_id==AUDIO_RX2)
	{
		lpf_prev = &DFM_RX2_lpf_prev;
		hpf_prev_a = &DFM_RX2_hpf_prev_a;
		hpf_prev_b = &DFM_RX2_hpf_prev_b;
		i_prev = &DFM_RX2_i_prev;
		q_prev = &DFM_RX2_q_prev;
		fm_sql_count = &DFM_RX2_fm_sql_count;
		fm_sql_avg = &DFM_RX2_fm_sql_avg;
		FPGA_Audio_Buffer_I_tmp = &FPGA_Audio_Buffer_RX2_I_tmp[0];
		FPGA_Audio_Buffer_Q_tmp = &FPGA_Audio_Buffer_RX2_Q_tmp[0];
	}
	
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		// first, calculate "x" and "y" for the arctan2, comparing the vectors of present data with previous data
		y = (FPGA_Audio_Buffer_Q_tmp[i] * *i_prev) - (FPGA_Audio_Buffer_I_tmp[i] * *q_prev);
		x = (FPGA_Audio_Buffer_I_tmp[i] * *i_prev) + (FPGA_Audio_Buffer_Q_tmp[i] * *q_prev);
		angle = atan2f(y, x);

		// we now have our audio in "angle"
		squelch_buf[i] = angle; // save audio in "d" buffer for squelch noise filtering/detection - done later

		a = *lpf_prev + (FM_RX_LPF_ALPHA * (angle - *lpf_prev)); //
		*lpf_prev = a;										   // save "[n-1]" sample for next iteration

		*q_prev = FPGA_Audio_Buffer_Q_tmp[i]; // save "previous" value of each channel to allow detection of the change of angle in next go-around
		*i_prev = FPGA_Audio_Buffer_I_tmp[i];

		if ((!TRX_Squelched) || (!TRX.FM_SQL_threshold)) // high-pass audio only if we are un-squelched (to save processor time)
		{
			if (CurrentVFO()->Mode == TRX_MODE_WFM)
			{
				FPGA_Audio_Buffer_I_tmp[i] = (float32_t)(angle / PI * (1 << 14)); //second way
			}
			else
			{
				b = FM_RX_HPF_ALPHA * (*hpf_prev_b + a - *hpf_prev_a); // do differentiation
				*hpf_prev_a = a;										 // save "[n-1]" samples for next iteration
				*hpf_prev_b = b;
				FPGA_Audio_Buffer_I_tmp[i] = b * 30000.0f; // save demodulated and filtered audio in main audio processing buffer
			}
		}
		else if (TRX_Squelched)				// were we squelched or tone NOT detected?
			FPGA_Audio_Buffer_I_tmp[i] = 0; // do not filter receive audio - fill buffer with zeroes to mute it
	}

	// *** Squelch Processing ***
	//arm_iir_lattice_f32(&IIR_Squelch_HPF, squelch_buf, squelch_buf, FPGA_AUDIO_BUFFER_HALF_SIZE);	// Do IIR high-pass filter on audio so we may detect squelch noise energy
	*fm_sql_avg = ((1.0f - FM_RX_SQL_SMOOTHING) * *fm_sql_avg) + (FM_RX_SQL_SMOOTHING * sqrtf(fabsf(squelch_buf[0]))); // IIR filter squelch energy magnitude:  We need look at only one representative sample

	// Squelch processing
	// Determine if the (averaged) energy in "ads.fm_sql_avg" is above or below the squelch threshold
	if (*fm_sql_count == 0) // do the squelch threshold calculation much less often than we are called to process this audio
	{
		if (*fm_sql_avg > 0.7f) // limit maximum noise value in averaging to keep it from going out into the weeds under no-signal conditions (higher = noisier)
			*fm_sql_avg = 0.7f;
		b = *fm_sql_avg * 10.0f; // scale noise amplitude to range of squelch setting
		// Now evaluate noise power with respect to squelch setting
		if (!TRX.FM_SQL_threshold) // is squelch set to zero?
			TRX_Squelched = false; // yes, the we are un-squelched
		else if (TRX_Squelched)	// are we squelched?
		{
			if (b <= (float)((10 - TRX.FM_SQL_threshold) - FM_SQUELCH_HYSTERESIS)) // yes - is average above threshold plus hysteresis?
				TRX_Squelched = false;											   //  yes, open the squelch
		}
		else // is the squelch open (e.g. passing audio)?
		{
			if ((10.0f - TRX.FM_SQL_threshold) > FM_SQUELCH_HYSTERESIS) // is setting higher than hysteresis?
			{
				if (b > (float)((10 - TRX.FM_SQL_threshold) + FM_SQUELCH_HYSTERESIS)) // yes - is average below threshold minus hysteresis?
					TRX_Squelched = true;											  // yes, close the squelch
			}
			else // setting is lower than hysteresis so we can't use it!
			{
				if (b > (10.0f - (float)TRX.FM_SQL_threshold)) // yes - is average below threshold?
					TRX_Squelched = true;					   // yes, close the squelch
			}
		}
		//
		(*fm_sql_count)++; // bump count that controls how often the squelch threshold is checked
		if (*fm_sql_count >= FM_SQUELCH_PROC_DECIMATION)
			*fm_sql_count = 0; // enforce the count limit
	}
}

static void ModulateFM(void)
{
	static uint32_t modulation = TRX_SAMPLERATE;
	static float32_t hpf_prev_a = 0;
	static float32_t hpf_prev_b = 0;
	static float32_t sin_data = 0;
	static uint32_t fm_mod_accum = 0;
	static float32_t modulation_index = 2.0f;
	if (CurrentVFO()->LPF_Filter_Width == 5000)
		modulation_index = 2.0f;
	if (CurrentVFO()->LPF_Filter_Width == 6000)
		modulation_index = 3.0f;
	if (CurrentVFO()->LPF_Filter_Width == 7000)
		modulation_index = 4.0f;
	if (CurrentVFO()->LPF_Filter_Width == 8000)
		modulation_index = 5.0f;
	if (CurrentVFO()->LPF_Filter_Width == 9000)
		modulation_index = 6.0f;
	if (CurrentVFO()->LPF_Filter_Width == 10000)
		modulation_index = 7.0f;
	if (CurrentVFO()->LPF_Filter_Width == 15000)
		modulation_index = 8.0f;
	if (CurrentVFO()->LPF_Filter_Width == 20000)
		modulation_index = 9.0f;
	if (CurrentVFO()->LPF_Filter_Width == 0)
		modulation_index = 10.0f;
	// Do differentiating high-pass filter to provide 6dB/octave pre-emphasis - which also removes any DC component!
	for (uint_fast16_t i = 0; i < FPGA_AUDIO_BUFFER_HALF_SIZE; i++)
	{
		float32_t a = FPGA_Audio_Buffer_TX_I_tmp[i];
		hpf_prev_b = FM_TX_HPF_ALPHA * (hpf_prev_b + a - hpf_prev_a); // do differentiation
		hpf_prev_a = a;												  // save "[n-1] samples for next iteration
		fm_mod_accum += hpf_prev_b;									  // save differentiated data in audio buffer // change frequency using scaled audio
		fm_mod_accum %= modulation;									  // limit range
		sin_data = (fm_mod_accum / (float32_t)modulation) * PI * modulation_index;
		FPGA_Audio_Buffer_TX_I_tmp[i] = Processor_selected_RFpower_amplitude * arm_sin_f32(sin_data);
		FPGA_Audio_Buffer_TX_Q_tmp[i] = Processor_selected_RFpower_amplitude * arm_cos_f32(sin_data);
	}
}
