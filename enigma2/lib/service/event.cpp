#include <lib/service/event.h>
#include <lib/base/estring.h>
#include <lib/base/encoding.h>
#include <lib/dvb/dvbtime.h>
#include <lib/dvb/idvb.h>
#include <dvbsi++/event_information_section.h>
#include <dvbsi++/short_event_descriptor.h>
#include <dvbsi++/extended_event_descriptor.h>
#include <dvbsi++/linkage_descriptor.h>
#include <dvbsi++/component_descriptor.h>
#include <dvbsi++/descriptor_tag.h>

#include <sys/types.h>
#include <fcntl.h>

// static members / methods
std::string eServiceEvent::m_language = "---";
std::string eServiceEvent::m_language_alternative = "---";

///////////////////////////

DEFINE_REF(eServiceEvent);
DEFINE_REF(eComponentData);

/* search for the presence of language from given EIT event descriptors*/
bool eServiceEvent::loadLanguage(Event *evt, std::string lang, int tsidonid)
{
	bool retval=0;
	for (DescriptorConstIterator desc = evt->getDescriptors()->begin(); desc != evt->getDescriptors()->end(); ++desc)
	{
		switch ((*desc)->getTag())
		{
			case LINKAGE_DESCRIPTOR:
				m_linkage_services.clear();
				break;
			case SHORT_EVENT_DESCRIPTOR:
			{
				const ShortEventDescriptor *sed = (ShortEventDescriptor*)*desc;
				std::string cc = sed->getIso639LanguageCode();
				std::transform(cc.begin(), cc.end(), cc.begin(), tolower);
				int table=encodingHandler.getCountryCodeDefaultMapping(cc);
				if ( lang == "---" || lang.find(cc) != -1)
				{
					m_event_name = replace_all(replace_all(convertDVBUTF8(sed->getEventName(), table, tsidonid), "\n", " "), "\t", " ");
					m_short_description = convertDVBUTF8(sed->getText(), table, tsidonid);
					retval=1;
				}
				break;
			}
			case EXTENDED_EVENT_DESCRIPTOR:
			{
				const ExtendedEventDescriptor *eed = (ExtendedEventDescriptor*)*desc;
				std::string cc = eed->getIso639LanguageCode();
				std::transform(cc.begin(), cc.end(), cc.begin(), tolower);
				int table=encodingHandler.getCountryCodeDefaultMapping(cc);
				if ( lang == "---" || lang.find(cc) != -1)
				{
					/*
					 * Bit of a hack, some providers put the event description partly in the short descriptor,
					 * and the remainder in extended event descriptors.
					 * In that case, we cannot really treat short/extended description as separate descriptions.
					 * Unfortunately we cannot recognise this, but we'll use the length of the short description
					 * to guess whether we should concatenate both descriptions (without any spaces)
					 */
					if (m_extended_description.empty() && m_short_description.size() >= 180)
					{
						m_extended_description = m_short_description;
						m_short_description = "";
					}
					m_extended_description += convertDVBUTF8(eed->getText(), table, tsidonid);
					retval=1;
				}
#if 0
				const ExtendedEventList *itemlist = eed->getItems();
				for (ExtendedEventConstIterator it = itemlist->begin(); it != itemlist->end(); ++it)
				{
					m_extended_description += '\n';
					m_extended_description += convertDVBUTF8((*it)->getItemDescription());
					m_extended_description += ' ';
					m_extended_description += convertDVBUTF8((*it)->getItem());
				}
#endif
				break;
			}
			default:
				break;
		}
	}
	if ( retval == 1 )
	{
		for (DescriptorConstIterator desc = evt->getDescriptors()->begin(); desc != evt->getDescriptors()->end(); ++desc)
		{
			switch ((*desc)->getTag())
			{
				case COMPONENT_DESCRIPTOR:
				{
					const ComponentDescriptor *cp = (ComponentDescriptor*)*desc;
					eComponentData data;
					data.m_streamContent = cp->getStreamContent();
					data.m_componentType = cp->getComponentType();
					data.m_componentTag = cp->getComponentTag();
					data.m_iso639LanguageCode = cp->getIso639LanguageCode();
					std::transform(data.m_iso639LanguageCode.begin(), data.m_iso639LanguageCode.end(), data.m_iso639LanguageCode.begin(), tolower);
					int table=encodingHandler.getCountryCodeDefaultMapping(data.m_iso639LanguageCode);
					data.m_text = convertDVBUTF8(cp->getText(),table,tsidonid);
					m_component_data.push_back(data);
					break;
				}
				case LINKAGE_DESCRIPTOR:
				{
					const LinkageDescriptor  *ld = (LinkageDescriptor*)*desc;
					if ( ld->getLinkageType() == 0xB0 )
					{
						eServiceReference ref;
						ref.type = eServiceReference::idDVB;
						eServiceReferenceDVB &dvb_ref = (eServiceReferenceDVB&) ref;
						dvb_ref.setServiceType(1);
						dvb_ref.setTransportStreamID(ld->getTransportStreamId());
						dvb_ref.setOriginalNetworkID(ld->getOriginalNetworkId());
						dvb_ref.setServiceID(ld->getServiceId());
						const PrivateDataByteVector *privateData = ld->getPrivateDataBytes();
						dvb_ref.name = convertDVBUTF8((const unsigned char*)&((*privateData)[0]), privateData->size(), 1, tsidonid);
						m_linkage_services.push_back(ref);
					}
					break;
				}
			}
		}
	}
	if ( m_extended_description.find(m_short_description) == 0 )
		m_short_description="";
	return retval;
}

RESULT eServiceEvent::parseFrom(Event *evt, int tsidonid)
{
	uint16_t stime_mjd = evt->getStartTimeMjd();
	uint32_t stime_bcd = evt->getStartTimeBcd();
	uint32_t duration = evt->getDuration();
	m_begin = parseDVBtime(
		stime_mjd >> 8,
		stime_mjd&0xFF,
		stime_bcd >> 16,
		(stime_bcd >> 8)&0xFF,
		stime_bcd & 0xFF
	);
	m_event_id = evt->getEventId();
	m_duration = fromBCD(duration>>16)*3600+fromBCD(duration>>8)*60+fromBCD(duration);
	if (m_language != "---" && loadLanguage(evt, m_language, tsidonid))
		return 0;
	if (m_language_alternative != "---" && loadLanguage(evt, m_language_alternative, tsidonid))
		return 0;
	if (loadLanguage(evt, "eng", tsidonid))
		return 0;
	if (loadLanguage(evt, "---", tsidonid))
		return 0;
	return 0;
}

RESULT eServiceEvent::parseFrom(const std::string& filename, int tsidonid)
{
	if (!filename.empty())
	{
		int fd = ::open( filename.c_str(), O_RDONLY );
		if ( fd > -1 )
		{
			__u8 buf[4096];
			int rd = ::read(fd, buf, 4096);
			::close(fd);
			if ( rd > 12 /*EIT_LOOP_SIZE*/ )
			{
				Event ev(buf);
				parseFrom(&ev, tsidonid);
				return 0;
			}
		}
	}
	return -1;
}

std::string eServiceEvent::getBeginTimeString() const
{
	tm t;
	localtime_r(&m_begin, &t);
	char tmp[13];
	snprintf(tmp, 13, "%02d.%02d, %02d:%02d",
		t.tm_mday, t.tm_mon+1,
		t.tm_hour, t.tm_min);
	return std::string(tmp, 12);
}

RESULT eServiceEvent::getComponentData(ePtr<eComponentData> &dest, int tagnum) const
{
	std::list<eComponentData>::const_iterator it =
		m_component_data.begin();
	for(;it != m_component_data.end(); ++it)
	{
		if ( it->m_componentTag == tagnum )
		{
			dest=new eComponentData(*it);
			return 0;
		}
	}
	dest=0;
	return -1;
}

PyObject *eServiceEvent::getComponentData() const
{
	ePyObject ret = PyList_New(m_component_data.size());
	int cnt=0;
	for (std::list<eComponentData>::const_iterator it(m_component_data.begin()); it != m_component_data.end(); ++it)
	{
		ePyObject tuple = PyTuple_New(5);
		PyTuple_SET_ITEM(tuple, 0, PyInt_FromLong(it->m_componentTag));
		PyTuple_SET_ITEM(tuple, 1, PyInt_FromLong(it->m_componentType));
		PyTuple_SET_ITEM(tuple, 2, PyInt_FromLong(it->m_streamContent));
		PyTuple_SET_ITEM(tuple, 3, PyString_FromString(it->m_iso639LanguageCode.c_str()));
		PyTuple_SET_ITEM(tuple, 4, PyString_FromString(it->m_text.c_str()));
		PyList_SET_ITEM(ret, cnt++, tuple);
	}
	return ret;
}

RESULT eServiceEvent::getLinkageService(eServiceReference &service, eServiceReference &parent, int num) const
{
	std::list<eServiceReference>::const_iterator it =
		m_linkage_services.begin();
	while( it != m_linkage_services.end() && num-- )
		++it;
	if ( it != m_linkage_services.end() )
	{
		service = *it;
		eServiceReferenceDVB &subservice = (eServiceReferenceDVB&) service;
		eServiceReferenceDVB &current = (eServiceReferenceDVB&) parent;
		subservice.setDVBNamespace(current.getDVBNamespace());
		if ( current.getParentTransportStreamID().get() )
		{
			subservice.setParentTransportStreamID( current.getParentTransportStreamID() );
			subservice.setParentServiceID( current.getParentServiceID() );
		}
		else
		{
			subservice.setParentTransportStreamID( current.getTransportStreamID() );
			subservice.setParentServiceID( current.getServiceID() );
		}
		if ( subservice.getParentTransportStreamID() == subservice.getTransportStreamID() &&
			subservice.getParentServiceID() == subservice.getServiceID() )
		{
			subservice.setParentTransportStreamID( eTransportStreamID(0) );
			subservice.setParentServiceID( eServiceID(0) );
		}
		return 0;
	}
	service.type = eServiceReference::idInvalid;
	return -1;
}

DEFINE_REF(eDebugClass);
