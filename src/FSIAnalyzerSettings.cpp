#include "FSIAnalyzerSettings.h"
#include <AnalyzerHelpers.h>

FSIAnalyzerSettings::FSIAnalyzerSettings()
    : mClockChannel( UNDEFINED_CHANNEL ),
      mDataChannel0( UNDEFINED_CHANNEL ),
      mDataChannel1( UNDEFINED_CHANNEL ),
      mTwoLane( false ),
      mNWordCount( 16 )
{
    mClockChannelInterface.reset( new AnalyzerSettingInterfaceChannel() );
    mClockChannelInterface->SetTitleAndTooltip( "TXCLK / RXCLK", "FSI clock line" );
    mClockChannelInterface->SetChannel( mClockChannel );

    mDataChannel0Interface.reset( new AnalyzerSettingInterfaceChannel() );
    mDataChannel0Interface->SetTitleAndTooltip( "TXD0 / RXD0",
        "FSI data lane 0 - always required" );
    mDataChannel0Interface->SetChannel( mDataChannel0 );

    mDataChannel1Interface.reset( new AnalyzerSettingInterfaceChannel() );
    mDataChannel1Interface->SetTitleAndTooltip( "TXD1 / RXD1 (2-lane only)",
        "FSI data lane 1 - only needed in 2-lane mode" );
    mDataChannel1Interface->SetChannel( mDataChannel1 );
    mDataChannel1Interface->SetSelectionOfNoneIsAllowed( true );

    mTwoLaneInterface.reset( new AnalyzerSettingInterfaceBool() );
    mTwoLaneInterface->SetTitleAndTooltip( "2-Lane Mode",
        "Enable dual-lane capture (TXD0 + TXD1). "
        "Even-numbered bits arrive on TXD0, odd-numbered bits on TXD1." );
    mTwoLaneInterface->SetValue( mTwoLane );

    mNWordCountInterface.reset( new AnalyzerSettingInterfaceNumberList() );
    mNWordCountInterface->SetTitleAndTooltip( "N-Word Frame Count (type 0x6)",
        "Number of 16-bit data words in N-word frames. "
        "Must match FSI_TX_FRAME_CTRL.N_WORDS in your firmware. "
        "Has no effect on fixed-size frames (PING/ERROR/DATA 1-6w)." );
    for( U32 n = 1; n <= 16; n++ )
    {
        char label[8];
        snprintf( label, sizeof(label), "%u", n );
        mNWordCountInterface->AddNumber( (double)n, label,
            n == 16 ? "Maximum (default)" : "" );
    }
    mNWordCountInterface->SetNumber( (double)mNWordCount );

    AddInterface( mClockChannelInterface.get() );
    AddInterface( mDataChannel0Interface.get() );
    AddInterface( mDataChannel1Interface.get() );
    AddInterface( mTwoLaneInterface.get() );
    AddInterface( mNWordCountInterface.get() );

    AddExportOption( 0, "Export as CSV" );
    AddExportExtension( 0, "CSV", "csv" );

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     false );
    AddChannel( mDataChannel0,  "TXD0/RXD0", false );
    AddChannel( mDataChannel1,  "TXD1/RXD1", false );
}

FSIAnalyzerSettings::~FSIAnalyzerSettings() {}

bool FSIAnalyzerSettings::SetSettingsFromInterfaces()
{
    mClockChannel = mClockChannelInterface->GetChannel();
    mDataChannel0 = mDataChannel0Interface->GetChannel();
    mDataChannel1 = mDataChannel1Interface->GetChannel();
    mTwoLane      = mTwoLaneInterface->GetValue();
    mNWordCount   = (U32)mNWordCountInterface->GetNumber();

    if( mClockChannel == UNDEFINED_CHANNEL )
    {
        SetErrorText( "Please select the clock channel (TXCLK/RXCLK)." );
        return false;
    }
    if( mDataChannel0 == UNDEFINED_CHANNEL )
    {
        SetErrorText( "Please select the TXD0/RXD0 data channel." );
        return false;
    }
    if( mTwoLane && mDataChannel1 == UNDEFINED_CHANNEL )
    {
        SetErrorText( "2-lane mode is enabled but TXD1/RXD1 channel is not assigned." );
        return false;
    }
    if( mNWordCount < 1 || mNWordCount > 16 )
    {
        SetErrorText( "N-Word count must be between 1 and 16." );
        return false;
    }

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     true );
    AddChannel( mDataChannel0,  "TXD0/RXD0", true );
    if( mTwoLane )
        AddChannel( mDataChannel1, "TXD1/RXD1", true );

    return true;
}

void FSIAnalyzerSettings::UpdateInterfacesFromSettings()
{
    mClockChannelInterface->SetChannel( mClockChannel );
    mDataChannel0Interface->SetChannel( mDataChannel0 );
    mDataChannel1Interface->SetChannel( mDataChannel1 );
    mTwoLaneInterface->SetValue( mTwoLane );
    mNWordCountInterface->SetNumber( (double)mNWordCount );
}

void FSIAnalyzerSettings::LoadSettings( const char* settings )
{
    SimpleArchive text_archive;
    text_archive.SetString( settings );
    text_archive >> mClockChannel;
    text_archive >> mDataChannel0;
    text_archive >> mDataChannel1;
    text_archive >> mTwoLane;
    text_archive >> mNWordCount;

    ClearChannels();
    AddChannel( mClockChannel,  "TXCLK",     true );
    AddChannel( mDataChannel0,  "TXD0/RXD0", true );
    if( mTwoLane )
        AddChannel( mDataChannel1, "TXD1/RXD1", true );

    UpdateInterfacesFromSettings();
}

const char* FSIAnalyzerSettings::SaveSettings()
{
    SimpleArchive text_archive;
    text_archive << mClockChannel;
    text_archive << mDataChannel0;
    text_archive << mDataChannel1;
    text_archive << mTwoLane;
    text_archive << mNWordCount;

    mSavedSettings = text_archive.GetString();
    return mSavedSettings.c_str();
}
