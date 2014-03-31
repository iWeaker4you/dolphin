// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// Games that uses this UCode:
// Zelda: The Windwaker, Mario Sunshine, Mario Kart, Twilight Princess,
// Super Mario Galaxy

#include "Core/ConfigManager.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/DSPHLE/UCodes/Zelda.h"


ZeldaUCode::ZeldaUCode(DSPHLE *dsphle, u32 crc)
	: UCodeInterface(dsphle, crc),
	  m_sync_in_progress(false),
	  m_max_voice(0),
	  m_num_sync_mail(0),
	  m_num_voices(0),
	  m_sync_cmd_pending(false),
	  m_current_voice(0),
	  m_current_buffer(0),
	  m_num_buffers(0),
	  m_num_steps(0),
	  m_list_in_progress(false),
	  m_step(0),
	  m_read_offset(0)
{
	DEBUG_LOG(DSPHLE, "UCode_Zelda - add boot mails for handshake");

	if (IsLightVersion())
	{
		DEBUG_LOG(DSPHLE, "Luigi Stylee!");
		m_mail_handler.PushMail(0x88881111);
	}
	else
	{
		m_mail_handler.PushMail(DSP_INIT);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
		m_mail_handler.PushMail(0xF3551111); // handshake
	}
}

ZeldaUCode::~ZeldaUCode()
{
	m_mail_handler.Clear();
}

void ZeldaUCode::Update()
{
	if (!IsLightVersion())
	{
		if (m_mail_handler.GetNextMail() == DSP_FRAME_END)
			DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
	}

	if (NeedsResumeMail())
	{
		m_mail_handler.PushMail(DSP_RESUME);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
	}
}

void ZeldaUCode::HandleMail(u32 mail)
{
	if (IsLightVersion())
		HandleMail_LightVersion(mail);
	else if (IsSMSVersion())
		HandleMail_SMSVersion(mail);
	else
		HandleMail_NormalVersion(mail);
}

void ZeldaUCode::HandleMail_LightVersion(u32 mail)
{
	//ERROR_LOG(DSPHLE, "Light version mail %08X, list in progress: %s, step: %i/%i",
	// mail, m_list_in_progress ? "yes":"no", m_step, m_num_steps);

	if (m_sync_cmd_pending)
	{
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);

		// TODO(delroth): Mix audio.

		m_current_buffer++;

		if (m_current_buffer == m_num_buffers)
		{
			m_sync_cmd_pending = false;
			DEBUG_LOG(DSPHLE, "Update the SoundThread to be in sync");
		}
		return;
	}

	if (!m_list_in_progress)
	{
		switch ((mail >> 24) & 0x7F)
		{
		case 0x00: m_num_steps = 1; break; // dummy
		case 0x01: m_num_steps = 5; break; // DsetupTable
		case 0x02: m_num_steps = 3; break; // DsyncFrame

		default:
			{
				m_num_steps = 0;
				PanicAlert("Zelda uCode (light version): unknown/unsupported command %02X", (mail >> 24) & 0x7F);
			}
			return;
		}

		m_list_in_progress = true;
		m_step = 0;
	}

	if (m_step >= sizeof(m_buffer) / 4)
		PanicAlert("m_step out of range");

	((u32*)m_buffer)[m_step] = mail;
	m_step++;

	if (m_step >= m_num_steps)
	{
		ExecuteList();
		m_list_in_progress = false;
	}
}

void ZeldaUCode::HandleMail_SMSVersion(u32 mail)
{
	if (m_sync_in_progress)
	{
		if (m_sync_cmd_pending)
		{
			m_sync_flags[(m_num_sync_mail << 1)    ] = mail >> 16;
			m_sync_flags[(m_num_sync_mail << 1) + 1] = mail & 0xFFFF;

			m_num_sync_mail++;
			if (m_num_sync_mail == 2)
			{
				m_num_sync_mail = 0;
				m_sync_in_progress = false;

				// TODO(delroth): Mix audio.

				m_current_buffer++;

				m_mail_handler.PushMail(DSP_SYNC);
				DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
				m_mail_handler.PushMail(0xF355FF00 | m_current_buffer);

				if (m_current_buffer == m_num_buffers)
				{
					m_mail_handler.PushMail(DSP_FRAME_END);
					// DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);

					m_sync_cmd_pending = false;
				}
			}
		}
		else
		{
			m_sync_in_progress = false;
		}

		return;
	}

	if (m_list_in_progress)
	{
		if (m_step >= sizeof(m_buffer) / 4)
			PanicAlert("m_step out of range");

		((u32*)m_buffer)[m_step] = mail;
		m_step++;

		if (m_step >= m_num_steps)
		{
			ExecuteList();
			m_list_in_progress = false;
		}

		return;
	}

	// Here holds: m_sync_in_progress == false && m_list_in_progress == false

	if (mail == 0)
	{
		m_sync_in_progress = true;
		m_num_sync_mail = 0;
	}
	else if ((mail >> 16) == 0)
	{
		m_list_in_progress = true;
		m_num_steps = mail;
		m_step = 0;
	}
	else if ((mail >> 16) == 0xCDD1) // A 0xCDD1000X mail should come right after we send a DSP_SYNCEND mail
	{
		// The low part of the mail tells the operation to perform
		// Seeing as every possible operation number halts the uCode,
		// except 3, that thing seems to be intended for debugging
		switch (mail & 0xFFFF)
		{
		case 0x0003: // Do nothing
			return;

		case 0x0000: // Halt
		case 0x0001: // Dump memory? and halt
		case 0x0002: // Do something and halt
			WARN_LOG(DSPHLE, "Zelda uCode(SMS version): received halting operation %04X", mail & 0xFFFF);
			return;

		default:     // Invalid (the real ucode would likely crash)
			WARN_LOG(DSPHLE, "Zelda uCode(SMS version): received invalid operation %04X", mail & 0xFFFF);
			return;
		}
	}
	else
	{
		WARN_LOG(DSPHLE, "Zelda uCode (SMS version): unknown mail %08X", mail);
	}
}

void ZeldaUCode::HandleMail_NormalVersion(u32 mail)
{
	// WARN_LOG(DSPHLE, "Zelda uCode: Handle mail %08X", mail);

	if (m_upload_setup_in_progress) // evaluated first!
	{
		PrepareBootUCode(mail);
		return;
	}

	if (m_sync_in_progress)
	{
		if (m_sync_cmd_pending)
		{
			u32 n = (mail >> 16) & 0xF;
			m_max_voice = (n + 1) << 4;
			m_sync_flags[n] = mail & 0xFFFF;
			m_sync_in_progress = false;

			m_current_voice = m_max_voice;

			if (m_current_voice >= m_num_voices)
			{
				// TODO(delroth): Mix audio.

				m_current_buffer++;

				m_mail_handler.PushMail(DSP_SYNC);
				DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
				m_mail_handler.PushMail(0xF355FF00 | m_current_buffer);

				m_current_voice = 0;

				if (m_current_buffer == m_num_buffers)
				{
					if (!IsDMAVersion()) // this is a hack... without it Pikmin 1 Wii/ Zelda TP Wii mail-s stopped
						m_mail_handler.PushMail(DSP_FRAME_END);
					//g_dspInitialize.pGenerateDSPInterrupt();

					m_sync_cmd_pending = false;
				}
			}
		}
		else
		{
			m_sync_in_progress = false;
		}

		return;
	}

	if (m_list_in_progress)
	{
		if (m_step >= sizeof(m_buffer) / 4)
			PanicAlert("m_step out of range");

		((u32*)m_buffer)[m_step] = mail;
		m_step++;

		if (m_step >= m_num_steps)
		{
			ExecuteList();
			m_list_in_progress = false;
		}

		return;
	}

	// Here holds: m_sync_in_progress == false && m_list_in_progress == false

	// Zelda-only mails:
	// - 0000XXXX - Begin list
	// - 00000000, 000X0000 - Sync mails
	// - CDD1XXXX - comes after DsyncFrame completed, seems to be debugging stuff

	if (mail == 0)
	{
		m_sync_in_progress = true;
	}
	else if ((mail >> 16) == 0)
	{
		m_list_in_progress = true;
		m_num_steps = mail;
		m_step = 0;
	}
	else if ((mail >> 16) == 0xCDD1) // A 0xCDD1000X mail should come right after we send a DSP_FRAME_END mail
	{
		// The low part of the mail tells the operation to perform
		// Seeing as every possible operation number halts the uCode,
		// except 3, that thing seems to be intended for debugging
		switch (mail & 0xFFFF)
		{
		case 0x0003: // Do nothing - continue normally
			return;

		case 0x0001: // accepts params to either DMA to iram and/or DRAM (used for hotbooting a new ucode)
			// TODO find a better way to protect from HLEMixer?
			m_upload_setup_in_progress = true;
			return;

		case 0x0002: // Let IROM play us off
			m_dsphle->SetUCode(UCODE_ROM);
			return;

		case 0x0000: // Halt
			WARN_LOG(DSPHLE, "Zelda uCode: received halting operation %04X", mail & 0xFFFF);
			return;

		default:     // Invalid (the real ucode would likely crash)
			WARN_LOG(DSPHLE, "Zelda uCode: received invalid operation %04X", mail & 0xFFFF);
			return;
		}
	}
	else
	{
		WARN_LOG(DSPHLE, "Zelda uCode: unknown mail %08X", mail);
	}
}

// zelda debug ..803F6418
void ZeldaUCode::ExecuteList()
{
	// begin with the list
	m_read_offset = 0;

	u32 cmd_mail = Read32();
	u32 command = (cmd_mail >> 24) & 0x7f;
	u32 sync;
	u32 extra_data = cmd_mail & 0xFFFF;

	if (IsLightVersion())
		sync = 0x62 + (command << 1); // seen in DSP_UC_Luigi.txt
	else
		sync = cmd_mail >> 16;

	DEBUG_LOG(DSPHLE, "==============================================================================");
	DEBUG_LOG(DSPHLE, "Zelda UCode - execute dlist (command: 0x%04x : sync: 0x%04x)", command, sync);

	switch (command)
	{
		// dummy
		case 0x00: break;

		// DsetupTable ... zelda ww jumps to 0x0095
		case 0x01:
			Read32(); Read32(); Read32(); Read32();
			break;

			// SyncFrame ... zelda ww jumps to 0x0243
		case 0x02:
			Read32(); Read32();
			if (IsLightVersion())
				break;
			else
				return;


		// Simply sends the sync messages
		case 0x03: break;

/*		case 0x04: break;   // dunno ... zelda ww jmps to 0x0580
		case 0x05: break;   // dunno ... zelda ww jmps to 0x0592
		case 0x06: break;   // dunno ... zelda ww jmps to 0x0469
		case 0x07: break;   // dunno ... zelda ww jmps to 0x044d
		case 0x08: break;   // Mixer ... zelda ww jmps to 0x0485
		case 0x09: break;   // dunno ... zelda ww jmps to 0x044d
 */

		// DsetDolbyDelay ... zelda ww jumps to 0x00b2
		case 0x0d:
		{
			u32 tmp = Read32();
			DEBUG_LOG(DSPHLE, "DSetDolbyDelay");
			DEBUG_LOG(DSPHLE, "DOLBY2_DELAY_BUF (size 0x960):      0x%08x", tmp);
		}
			break;

			// This opcode, in the SMG ucode, sets the base address for audio data transfers from main memory (using DMA).
			// In the Zelda ucode, it is dummy, because this ucode uses accelerator for audio data transfers.
		case 0x0e:
			Read32();
			break;

		// default ... zelda ww jumps to 0x0043
		default:
			PanicAlert("Zelda UCode - unknown command: %x (size %i)", command, m_num_steps);
			break;
	}

	// sync, we are ready
	if (IsLightVersion())
	{
		if (m_sync_cmd_pending)
			m_mail_handler.PushMail(0x80000000 | m_num_buffers); // after CMD_2
		else
			m_mail_handler.PushMail(0x80000000 | sync); // after CMD_0, CMD_1
	}
	else
	{
		m_mail_handler.PushMail(DSP_SYNC);
		DSP::GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
		m_mail_handler.PushMail(0xF3550000 | sync);
	}
}

u32 ZeldaUCode::GetUpdateMs()
{
	return SConfig::GetInstance().bWii ? 3 : 5;
}

void ZeldaUCode::DoState(PointerWrap &p)
{
	p.Do(m_sync_in_progress);
	p.Do(m_max_voice);
	p.Do(m_sync_flags);

	p.Do(m_num_sync_mail);

	p.Do(m_num_voices);

	p.Do(m_sync_cmd_pending);
	p.Do(m_current_voice);
	p.Do(m_current_buffer);
	p.Do(m_num_buffers);

	p.Do(m_num_steps);
	p.Do(m_list_in_progress);
	p.Do(m_step);
	p.Do(m_buffer);

	p.Do(m_read_offset);

	DoStateShared(p);
}
