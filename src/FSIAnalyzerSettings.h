// TI FSI Analyzer
// Author: Nima Eskandari
// A Saleae Logic 2 Low-Level Analyzer plugin for Texas Instruments Fast Serial Interface

#pragma once
#include <AnalyzerSettings.h>
#include <AnalyzerTypes.h>

class FSIAnalyzerSettings : public AnalyzerSettings
{
public:
    FSIAnalyzerSettings();
    virtual ~FSIAnalyzerSettings();

    virtual bool SetSettingsFromInterfaces();
    virtual void LoadSettings( const char* settings );
    virtual const char* SaveSettings();

    void UpdateInterfacesFromSettings();

    // Channels
    Channel mClockChannel;
    Channel mDataChannel0;      // TXD0 / RXD0
    Channel mDataChannel1;      // TXD1 / RXD1 (optional, 2-lane)

    // Protocol options
    bool    mTwoLane;           // true = 2-lane DDR interleaved mode
    U32     mNWordCount;        // word count for N-word frames (1-16)

protected:
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mClockChannelInterface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mDataChannel0Interface;
    std::unique_ptr<AnalyzerSettingInterfaceChannel>    mDataChannel1Interface;
    std::unique_ptr<AnalyzerSettingInterfaceBool>       mTwoLaneInterface;
    std::unique_ptr<AnalyzerSettingInterfaceNumberList> mNWordCountInterface;

private:
    std::string mSavedSettings;   // owns the string returned by SaveSettings()
};
