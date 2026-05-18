#include "FSIAnalyzerResults.h"
#include "FSIAnalyzer.h"
#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>
#include <fstream>
#include <sstream>

// Defined in FSIAnalyzer.cpp — shared label helper
extern const char* FrameTypeName( U64 ft );

FSIAnalyzerResults::FSIAnalyzerResults( FSIAnalyzer* analyzer,
                                        FSIAnalyzerSettings* settings )
    : AnalyzerResults(), mSettings( settings ), mAnalyzer( analyzer )
{
}

FSIAnalyzerResults::~FSIAnalyzerResults() {}

void FSIAnalyzerResults::GenerateBubbleText( U64 frame_index,
                                              Channel& /*channel*/,
                                              DisplayBase display_base )
{
    ClearResultStrings();
    Frame frame = GetFrame( frame_index );

    char number_str[64];

    switch( frame.mType )
    {
    case FSI_RESULT_PREAMBLE:
        AddResultString( "PRE" );
        AddResultString( "Preamble" );
        break;

    case FSI_RESULT_FRAME_TYPE:
        AddResultString( "FT" );
        AddResultString( FrameTypeName( frame.mData1 ) );
        break;

    case FSI_RESULT_TAG:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 4, number_str, 64 );
        AddResultString( "TAG" );
        { std::string s = std::string("Tag: ") + number_str; AddResultString( s.c_str() ); }
        break;

    case FSI_RESULT_USERDATA:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        AddResultString( "UD" );
        { std::string s = std::string("UserData: ") + number_str; AddResultString( s.c_str() ); }
        break;

    case FSI_RESULT_DATA_WORD:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );
        {
            std::string short_s = std::string("D") + number_str;
            std::string long_s  = std::string("Data[") + std::to_string(frame.mData2) + "]: " + number_str;
            AddResultString( short_s.c_str() );
            AddResultString( long_s.c_str() );
        }
        break;

    case FSI_RESULT_CRC:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        {
            std::string s = std::string("CRC: ") + number_str;
            bool crc_ok = ( frame.mFlags & 0x01 ) != 0;
            if( !crc_ok ) s += " [BAD]";
            AddResultString( "CRC" );
            AddResultString( s.c_str() );
        }
        break;

    case FSI_RESULT_EOF:
        AddResultString( "EOF" );
        AddResultString( "End of Frame" );
        break;

    case FSI_RESULT_SOF:
        AddResultString( "SOF" );
        AddResultString( "Start of Frame" );
        break;

    case FSI_RESULT_POSTAMBLE:
        AddResultString( "POST" );
        AddResultString( "Postamble" );
        break;

    case FSI_RESULT_ERROR:
        AddResultString( "ERR" );
        AddResultString( "Framing Error" );
        break;
    }
}

void FSIAnalyzerResults::GenerateExportFile( const char* file,
                                              DisplayBase display_base,
                                              U32 /*export_type_user_id*/ )
{
    std::ofstream file_stream( file, std::ios::out );
    file_stream << "Frame Index,Type,Value,Word Index,CRC OK,Start Sample,End Sample\n";

    U64 num_frames = GetNumFrames();
    char number_str[64];

    for( U64 i = 0; i < num_frames; i++ )
    {
        Frame frame = GetFrame( i );
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );

        file_stream << i << ",";

        switch( frame.mType )
        {
        case FSI_RESULT_PREAMBLE:   file_stream << "Preamble,,,,"; break;
        case FSI_RESULT_FRAME_TYPE: file_stream << "FrameType," << FrameTypeName( frame.mData1 ) << ",,,"; break;
        case FSI_RESULT_TAG:        file_stream << "Tag," << number_str << ",,,"; break;
        case FSI_RESULT_USERDATA:   file_stream << "UserData," << number_str << ",,,"; break;
        case FSI_RESULT_DATA_WORD:
            file_stream << "DataWord," << number_str << "," << frame.mData2 << ",,";
            break;
        case FSI_RESULT_CRC:
            file_stream << "CRC," << number_str << ",,"
                        << ( ( frame.mFlags & 0x01 ) ? "OK" : "FAIL" ) << ",";
            break;
        case FSI_RESULT_EOF:       file_stream << "EOF,,,,"; break;
        case FSI_RESULT_SOF:       file_stream << "SOF,,,,"; break;
        case FSI_RESULT_POSTAMBLE: file_stream << "Postamble,,,,"; break;
        case FSI_RESULT_ERROR:     file_stream << "Error,,,,"; break;
        default:                   file_stream << "Unknown,,,,"; break;
        }

        file_stream << frame.mStartingSampleInclusive << ","
                    << frame.mEndingSampleInclusive << "\n";

        if( UpdateExportProgressAndCheckForCancel( i, num_frames ) )
            return;
    }

    file_stream.close();
}

void FSIAnalyzerResults::GenerateFrameTabularText( U64 frame_index,
                                                    DisplayBase display_base )
{
    ClearTabularText();
    Frame frame = GetFrame( frame_index );
    char number_str[64];

    switch( frame.mType )
    {
    case FSI_RESULT_PREAMBLE:
        AddTabularText( "Preamble" );
        break;
    case FSI_RESULT_FRAME_TYPE:
        AddTabularText( FrameTypeName( frame.mData1 ) );
        break;
    case FSI_RESULT_TAG:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 4, number_str, 64 );
        { std::string s = std::string("Tag=") + number_str; AddTabularText( s.c_str() ); }
        break;
    case FSI_RESULT_USERDATA:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        { std::string s = std::string("UserData=") + number_str; AddTabularText( s.c_str() ); }
        break;
    case FSI_RESULT_DATA_WORD:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 16, number_str, 64 );
        {
            std::string s = std::string("D[") + std::to_string(frame.mData2) + "]=" + number_str;
            AddTabularText( s.c_str() );
        }
        break;
    case FSI_RESULT_CRC:
        AnalyzerHelpers::GetNumberString( frame.mData1, display_base, 8, number_str, 64 );
        {
            std::string s = std::string("CRC=") + number_str +
                            ( ( frame.mFlags & 0x01 ) ? " OK" : " BAD" );
            AddTabularText( s.c_str() );
        }
        break;
    case FSI_RESULT_EOF:
        AddTabularText( "EOF" );
        break;
    case FSI_RESULT_SOF:
        AddTabularText( "SOF" );
        break;
    case FSI_RESULT_POSTAMBLE:
        AddTabularText( "Postamble" );
        break;
    case FSI_RESULT_ERROR:
        AddTabularText( "FRAMING ERROR" );
        break;
    }
}

void FSIAnalyzerResults::GeneratePacketTabularText( U64, DisplayBase ) { ClearTabularText(); }
void FSIAnalyzerResults::GenerateTransactionTabularText( U64, DisplayBase ) { ClearTabularText(); }
