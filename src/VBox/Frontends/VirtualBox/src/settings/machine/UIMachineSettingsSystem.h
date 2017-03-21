/* $Id$ */
/** @file
 * VBox Qt GUI - UIMachineSettingsSystem class declaration.
 */

/*
 * Copyright (C) 2008-2017 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___UIMachineSettingsSystem_h___
#define ___UIMachineSettingsSystem_h___

/* GUI includes: */
#include "UISettingsPage.h"
#include "UIMachineSettingsSystem.gen.h"


/** Machine settings: System Boot data structure. */
struct UIBootItemData
{
    /** Constructs data. */
    UIBootItemData()
        : m_type(KDeviceType_Null)
        , m_fEnabled(false)
    {}

    /** Returns whether the @a other passed data is equal to this one. */
    bool operator==(const UIBootItemData &other) const
    {
        return true
               && (m_type == other.m_type)
               && (m_fEnabled == other.m_fEnabled)
               ;
    }

    /** Holds the boot device type. */
    KDeviceType m_type;
    /** Holds whether the boot device enabled. */
    bool m_fEnabled;
};


/** Machine settings: System page data structure. */
struct UIDataSettingsMachineSystem
{
    /** Constructs data. */
    UIDataSettingsMachineSystem()
        /* Support flags: */
        : m_fSupportedPAE(false)
        , m_fSupportedHwVirtEx(false)
        /* Motherboard data: */
        , m_iMemorySize(-1)
        , m_bootItems(QList<UIBootItemData>())
        , m_chipsetType(KChipsetType_Null)
        , m_pointingHIDType(KPointingHIDType_None)
        , m_fEnabledIoApic(false)
        , m_fEnabledEFI(false)
        , m_fEnabledUTC(false)
        /* CPU data: */
        , m_cCPUCount(-1)
        , m_iCPUExecCap(-1)
        , m_fEnabledPAE(false)
        /* Acceleration data: */
        , m_paravirtProvider(KParavirtProvider_None)
        , m_fEnabledHwVirtEx(false)
        , m_fEnabledNestedPaging(false)
    {}

    /** Returns whether the @a other passed data is equal to this one. */
    bool equal(const UIDataSettingsMachineSystem &other) const
    {
        return true
               /* Support flags: */
               && (m_fSupportedPAE == other.m_fSupportedPAE)
               && (m_fSupportedHwVirtEx == other.m_fSupportedHwVirtEx)
               /* Motherboard data: */
               && (m_iMemorySize == other.m_iMemorySize)
               && (m_bootItems == other.m_bootItems)
               && (m_chipsetType == other.m_chipsetType)
               && (m_pointingHIDType == other.m_pointingHIDType)
               && (m_fEnabledIoApic == other.m_fEnabledIoApic)
               && (m_fEnabledEFI == other.m_fEnabledEFI)
               && (m_fEnabledUTC == other.m_fEnabledUTC)
               /* CPU data: */
               && (m_cCPUCount == other.m_cCPUCount)
               && (m_iCPUExecCap == other.m_iCPUExecCap)
               && (m_fEnabledPAE == other.m_fEnabledPAE)
               /* Acceleration data: */
               && (m_paravirtProvider == other.m_paravirtProvider)
               && (m_fEnabledHwVirtEx == other.m_fEnabledHwVirtEx)
               && (m_fEnabledNestedPaging == other.m_fEnabledNestedPaging)
               ;
    }

    /** Returns whether the @a other passed data is equal to this one. */
    bool operator==(const UIDataSettingsMachineSystem &other) const { return equal(other); }
    /** Returns whether the @a other passed data is different from this one. */
    bool operator!=(const UIDataSettingsMachineSystem &other) const { return !equal(other); }

    /** Holds whether the PAE is supported. */
    bool  m_fSupportedPAE;
    /** Holds whether the HW Virt Ex is supported. */
    bool  m_fSupportedHwVirtEx;

    /** Holds the RAM size. */
    int                    m_iMemorySize;
    /** Holds the boot items. */
    QList<UIBootItemData>  m_bootItems;
    /** Holds the chipset type. */
    KChipsetType           m_chipsetType;
    /** Holds the pointing HID type. */
    KPointingHIDType       m_pointingHIDType;
    /** Holds whether the IO APIC is enabled. */
    bool                   m_fEnabledIoApic;
    /** Holds whether the EFI is enabled. */
    bool                   m_fEnabledEFI;
    /** Holds whether the UTC is enabled. */
    bool                   m_fEnabledUTC;

    /** Holds the CPU count. */
    int   m_cCPUCount;
    /** Holds the CPU execution cap. */
    int   m_iCPUExecCap;
    /** Holds whether the PAE is enabled. */
    bool  m_fEnabledPAE;

    /** Holds the paravirtualization provider. */
    KParavirtProvider  m_paravirtProvider;
    /** Holds whether the HW Virt Ex is enabled. */
    bool               m_fEnabledHwVirtEx;
    /** Holds whether the Nested Paging is enabled. */
    bool               m_fEnabledNestedPaging;
};
typedef UISettingsCache<UIDataSettingsMachineSystem> UISettingsCacheMachineSystem;


/** Machine settings: System page. */
class UIMachineSettingsSystem : public UISettingsPageMachine,
                                public Ui::UIMachineSettingsSystem
{
    Q_OBJECT;

public:

    /* Constructor: */
    UIMachineSettingsSystem();

    /* API: Correlation stuff: */
    bool isHWVirtExEnabled() const;
    bool isHIDEnabled() const;
    KChipsetType chipsetType() const;
    void setUSBEnabled(bool fEnabled);

protected:

    /** Returns whether the page content was changed. */
    bool changed() const { return m_cache.wasChanged(); }

    /** Loads data into the cache from corresponding external object(s),
      * this task COULD be performed in other than the GUI thread. */
    void loadToCacheFrom(QVariant &data);
    /** Loads data into corresponding widgets from the cache,
      * this task SHOULD be performed in the GUI thread only. */
    void getFromCache();

    /** Saves data from corresponding widgets to the cache,
      * this task SHOULD be performed in the GUI thread only. */
    void putToCache();
    /** Saves data from the cache to corresponding external object(s),
      * this task COULD be performed in other than the GUI thread. */
    void saveFromCacheTo(QVariant &data);

    /** Performs validation, updates @a messages list if something is wrong. */
    bool validate(QList<UIValidationMessage> &messages);

    /** Defines TAB order. */
    void setOrderAfter(QWidget *pWidget);

    /** Handles translation event. */
    void retranslateUi();

    /** Performs final page polishing. */
    void polishPage();

private slots:

    /* Handlers: Memory-size stuff: */
    void sltHandleMemorySizeSliderChange();
    void sltHandleMemorySizeEditorChange();

    /* Handler: Boot-table stuff: */
    void sltCurrentBootItemChanged(int iCurrentIndex);

    /* Handlers: CPU stuff: */
    void sltHandleCPUCountSliderChange();
    void sltHandleCPUCountEditorChange();
    void sltHandleCPUExecCapSliderChange();
    void sltHandleCPUExecCapEditorChange();

private:

    /* Helpers: Prepare stuff: */
    void prepare();
    void prepareTabMotherboard();
    void prepareTabProcessor();
    void prepareTabAcceleration();
    void prepareValidation();

    /* Helper: Pointing HID type combo stuff: */
    void repopulateComboPointingHIDType();

    /* Helpers: Translation stuff: */
    void retranslateComboChipsetType();
    void retranslateComboPointingHIDType();
    void retranslateComboParavirtProvider();

    /* Helper: Boot-table stuff: */
    void adjustBootOrderTWSize();

    /* Handler: Event-filtration stuff: */
    bool eventFilter(QObject *aObject, QEvent *aEvent);

    /* Variable: Boot-table stuff: */
    QList<KDeviceType> m_possibleBootItems;

    /* Variables: CPU stuff: */
    uint m_uMinGuestCPU;
    uint m_uMaxGuestCPU;
    uint m_uMinGuestCPUExecCap;
    uint m_uMedGuestCPUExecCap;
    uint m_uMaxGuestCPUExecCap;

    /* Variable: Correlation stuff: */
    bool m_fIsUSBEnabled;

    /* Cache: */
    UISettingsCacheMachineSystem m_cache;
};

#endif /* !___UIMachineSettingsSystem_h___ */

