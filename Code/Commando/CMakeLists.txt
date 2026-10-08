set(Combat_SOURCES
    combatgmode.cpp
    commandosaveload.cpp
    comnetrcvinst.cpp
    gameinitmgr.cpp
    god.cpp
    inputconfig.cpp
    inputconfigmgr.cpp
    level.cpp
    nethandler.cpp
    player.cpp
    playermanager.cpp
    regbool.cpp
    regfloat.cpp
    regint.cpp
    regstring.cpp
    renegadeplayerterminal.cpp
    systemsettings.cpp
    team.cpp
    teammanager.cpp
)
set(GameModes_SOURCES
    gamedata.cpp
    gdcnc.cpp
    gdsingleplayer.cpp
    gdskirmish.cpp
)
set(GameSpy_SOURCES
    CDKeyAuth.cpp
    GameSpy_QnR.cpp
    GameSpyBanList.cpp
    gamespyadmin.cpp
    gamespyauthmgr.cpp
)
set(GUI_SOURCES
    DlgDownload.cpp
    DlgMPConnect.cpp
    DlgMPConnectionRefused.cpp
    DlgMPTeamSelect.cpp
    DlgMessageBox.cpp
    DlgPasswordPrompt.cpp
    DlgQuickmatch.cpp
    DlgRestart.cpp
    DlgWOLAutoStart.cpp
    DlgWOLLogon.cpp
    DlgWOLSettings.cpp
    DlgWOLWait.cpp
    DlgWebPage.cpp
    dialogtests.cpp
    dlgcharacteroptions.cpp
    dlgcheatoptions.cpp
    dlgcncbattleinfo.cpp
    dlgcncpurchasemainmenu.cpp
    dlgcncpurchasemenu.cpp
    dlgcncreference.cpp
    dlgcncserverinfo.cpp
    dlgcncteaminfo.cpp
    dlgcncwinscreen.cpp
    dlgconfigaudiotab.cpp
    dlgconfigperformancetab.cpp
    dlgconfigvideotab.cpp
    dlgcontrols.cpp
    dlgcontrolsaveload.cpp
    dlgcontrolslisttab.cpp
    dlgcontroltabs.cpp
    dlgcredits.cpp
    dlgevabuildingstab.cpp
    dlgevacharacterstab.cpp
    dlgevadatatab.cpp
    dlgevaencyclopedia.cpp
    dlgevamaptab.cpp
    dlgevaobjectivestab.cpp
    dlgevavehiclestab.cpp
    dlgevaviewertab.cpp
    dlgevaweaponstab.cpp
    dlghelpscreen.cpp
    # dlginternetoptions.cpp
    dlgloadspgame.cpp
    dlgmainmenu.cpp
    dlgmovieoptions.cpp
    dlgmpchangelannickname.cpp
    dlgmpingamechat.cpp
    dlgmpingameteamchat.cpp
    dlgmplangamelist.cpp
    dlgmplanhostoptions.cpp
    dlgmpslaveservers.cpp
    dlgmpwoladdbuddy.cpp
    dlgmpwoladdignoreentry.cpp
    dlgmpwolautologinprompt.cpp
    dlgmpwolbuddies.cpp
    dlgmpwolbuddylistpopup.cpp
    dlgmpwolchat.cpp
    dlgmpwoldeletebuddy.cpp
    dlgmpwoldeleteignoreentry.cpp
    dlgmpwolgamelist.cpp
    dlgmpwolgoodies.cpp
    dlgmpwolhostchatlobby.cpp
    dlgmpwolignorelist.cpp
    dlgmpwolinvitation.cpp
    dlgmpwollocalenag.cpp
    dlgmpwolmain.cpp
    dlgmpwolmotd.cpp
    dlgmpwolpagebuddy.cpp
    dlgmpwolpagereply.cpp
    # dlgmpwolquickmatch.cpp
    dlgmpwolquickmatchoptions.cpp
    dlgmultiplayoptions.cpp
    dlgpreviewoptions.cpp
    dlgsavegame.cpp
    dlgserversaveload.cpp
    dlgsidebarhelp.cpp
    dlgtechoptions.cpp
    mainmenutransition.cpp
    radiocommanddisplay.cpp
    renegadedialogmgr.cpp
    skinpackage.cpp
    skinpackagemgr.cpp
    WebBrowser.cpp
)
if(REBEL_PORTABLE)
    # IE embedding is Windows-only; the web build links a browser-less stand-in.
    list(REMOVE_ITEM GUI_SOURCES WebBrowser.cpp)
    list(APPEND GUI_SOURCES ${PROJECT_SOURCE_DIR}/Code/Platform/media/webbrowser_web.cpp)
endif()
set(LANWOL_SOURCES
    lanchat.cpp
    chatshre.cpp
    gamechanlist.cpp
    gamechannel.cpp
    gamesideservercontrol.cpp
    netinterface.cpp
    ServerSettings.cpp
    slavemaster.cpp
)
set(Modes_SOURCES
    campaign.cpp
    console.cpp
    consolefunction.cpp
    gamemenu.cpp
    langmode.cpp
    movie.cpp
    overlay.cpp
    scorescreen.cpp
    textdisplay.cpp
    wolgmode.cpp
)
set(NetEvenets_SOURCES
    AnnounceEvent.cpp
    bioevent.cpp
    changeteamevent.cpp
    clientbboevent.cpp
    clientgoodbyeevent.cpp
    consolecommandevent.cpp
    csconsolecommandevent.cpp
    cshint.cpp
    cspingrequestevent.cpp
    cstextobj.cpp
    donateevent.cpp
    evictionevent.cpp
    gamedataupdateevent.cpp
    gameoptionsevent.cpp
    gamespycschallengeresponseevent.cpp
    gamespyscchallengeevent.cpp
    godmodeevent.cpp
    loadingevent.cpp
    moneyevent.cpp
    netevent.cpp
    playerkill.cpp
    purchaserequestevent.cpp
    purchaseresponseevent.cpp
    requestkillevent.cpp
    scoreevent.cpp
    scpingresponseevent.cpp
    sctextobj.cpp
    suicideevent.cpp
    svrgoodbyeevent.cpp
    vipmodeevent.cpp
    warpevent.cpp
    winevent.cpp
)
set(NetObjects_SOURCES
    clientfps.cpp
    serverfps.cpp
)
set(Network_SOURCES
    apppacketstats.cpp
    bandwidth.cpp
    bandwidthgraph.cpp
    clienthintmanager.cpp
    clientpingmanager.cpp
    cnetwork.cpp
    datasafe.cpp
    devoptions.cpp
    diagnostics.cpp
    messages.cpp
    multihud.cpp
    NAT.cpp
    nataddr.cpp
    natsock.cpp
    natter.cpp
    netgraphs.cpp
    nicenum.cpp
    pkthandlers.cpp
    priority.cpp
    sbbomanager.cpp
    useroptions.cpp
    vendor.cpp
)
set(Shell_SOURCES
    AutoStart.cpp
    ConsoleMode.cpp
    WINMAIN.CPP
    buildinfo.cpp
    cdverify.cpp
    cominit.cpp
    gamemode.cpp
    init.cpp
    mainloop.cpp
    shutdown.cpp
    singletoninstancekeeper.cpp
)
set(Unclaimed_SOURCES
    floodprotectionmgr.cpp
    modpackage.cpp
    modpackagemgr.cpp
    renegadecheatmgr.cpp
)
set(WOL_SOURCES
    FirewallWait.cpp
    GameResSend.cpp
    WOLBuddyMgr.cpp
    WOLChatMgr.cpp
    WOLDiags.cpp
    WOLGameInfo.cpp
    WOLJoinGame.cpp
    WOLLoginProfile.cpp
    WOLLogonMgr.cpp
    WOLQuickMatch.cpp
    bandwidthcheck.cpp
    mpsettingsmgr.cpp
    wollocalemgr.cpp
)
set(WWOnline_SOURCES
    ../WWOnline/GameResField.cpp
    ../WWOnline/GameResPacket.cpp
    ../WWOnline/PingProfile.cpp
    ../WWOnline/WOLChannel.cpp
    ../WWOnline/WOLChatMsg.cpp
    ../WWOnline/WOLChatObserver.cpp
    ../WWOnline/WOLConnect.cpp
    ../WWOnline/WOLDownload.cpp
    ../WWOnline/WOLErrorUtil.cpp
    ../WWOnline/WOLGame.cpp
    ../WWOnline/WOLGameOptions.cpp
    ../WWOnline/WOLLadder.cpp
    ../WWOnline/WOLLoginInfo.cpp
    ../WWOnline/WOLNetUtilObserver.cpp
    ../WWOnline/WOLPageMsg.cpp
    ../WWOnline/WOLProduct.cpp
    ../WWOnline/WOLServer.cpp
    ../WWOnline/WOLSession.cpp
    ../WWOnline/WOLSquad.cpp
    ../WWOnline/WOLString.cpp
    ../WWOnline/WOLUser.cpp
    ../WWOnline/WaitCondition.cpp
)
set(WOLBrowser_SOURCES
    ../WOLBrowser/WOLBrowser_i.c
)
set(WOLAPI_SOURCES
    ../WOLAPI/WOLAPI_i.c
)

add_executable(Renegade WIN32
    ${Combat_SOURCES}
    ${GameModes_SOURCES}
    ${GameSpy_SOURCES}
    ${GUI_SOURCES}
    ${LANWOL_SOURCES}
    ${Modes_SOURCES}
    ${NetEvenets_SOURCES}
    ${NetObjects_SOURCES}
    ${Network_SOURCES}
    ${Shell_SOURCES}
    ${Unclaimed_SOURCES}
    ${WOL_SOURCES}
    ${WOLAPI_SOURCES}
    ${WOLBrowser_SOURCES}
    ${WWOnline_SOURCES}
    chat.rc
)
target_include_directories(Renegade PUBLIC
    $<BUILD_INTERFACE:
        ${PROJECT_SOURCE_DIR}/Code/Commando
        ${PROJECT_SOURCE_DIR}/Code/WOLAPI
        ${PROJECT_SOURCE_DIR}/Code/WOLBrowser
        ${PROJECT_SOURCE_DIR}/Code
    >
    PRIVATE
        ${PROJECT_BINARY_DIR}/Code/Commando
        ${MSS_INCLUDE_DIRS}
)
target_link_libraries(Renegade PRIVATE
    BandTest
    BinkMovie
    Combat
    SControl
    WW3D2
    WWDebug
    WWLib
    WWMath
    WWNet
    WWPhys
    WWSaveLoad
    WWTranslateDb
    WWUi
    ${MSS_LIBRARIES}
    Microsoft::D3DX9
    dinput8
    version
    ws2_32
)
target_compile_definitions(Renegade PRIVATE
    WINDOWS_IGNORE_PACKING_MISMATCH
)
add_dependencies(Renegade GetGitHash Scripts)
set_target_properties(Renegade PROPERTIES
    OUTPUT_NAME_DEBUG "RenegadeD"
    OUTPUT_NAME_RELWITHDEBINFO "RenegadeP"
)

if(REBEL_PORTABLE)
    # Dialog templates: compile chat.rc to a Win32 .res and embed it; the
    # FindResource/LoadResource replacements read it at /rebel/chat.res.
    find_program(REBEL_LLVM_RC llvm-rc HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
    set(REBEL_CHAT_RES ${CMAKE_CURRENT_BINARY_DIR}/chat.res)
    add_custom_command(
        OUTPUT ${REBEL_CHAT_RES}
        COMMAND ${REBEL_LLVM_RC} /C 1252 /D _WIN32 /D NDEBUG
                /I ${CMAKE_CURRENT_SOURCE_DIR} /I ${PROJECT_SOURCE_DIR}/Code/Platform/rc-include
                /fo ${REBEL_CHAT_RES} chat.rc
        # relative input: llvm-rc reads arguments starting with '/' as options
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS chat.rc resource.h dialogresource.h ${PROJECT_SOURCE_DIR}/Code/Platform/rc-include/afxres.h
        COMMENT "Compiling dialog resources (chat.rc)"
    )
    add_custom_target(RenegadeResources DEPENDS ${REBEL_CHAT_RES})
    add_dependencies(Renegade RenegadeResources)
    target_link_options(Renegade PRIVATE "SHELL:--embed-file ${REBEL_CHAT_RES}@/rebel/chat.res")
endif()

if(REBEL_PORTABLE)
    # Browser entry point and link setup for the game.
    target_sources(Renegade PRIVATE ${PROJECT_SOURCE_DIR}/Code/Platform/web/main_web.cpp ${PROJECT_SOURCE_DIR}/Code/Platform/web/debug_web.cpp)
    # The engine relies on self-registering statics everywhere (persist and
    # definition factories, save/load subsystems, network object factories,
    # scripts). Objects reachable only through those registrations would be
    # dropped from static libraries, so link every engine library in full.
    set(REBEL_WHOLE_LIBS Scripts Combat WW3D2 WWPhys WWUi WWAudio WWNet WWSaveLoad WWTranslateDb WWLib WWMath
                         WWDebug WWBitPack WWUtil BinkMovie SControl BandTest)
    set(_whole "")
    foreach(_lib ${REBEL_WHOLE_LIBS})
        string(APPEND _whole " $<TARGET_FILE:${_lib}>")
    endforeach()
    target_link_options(Renegade PRIVATE "SHELL:-Wl,--whole-archive${_whole} -Wl,--no-whole-archive")
    target_link_options(Renegade PRIVATE
        -sWASMFS
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=262144
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=268435456
        -sMAXIMUM_MEMORY=4294967296
        -sSTACK_SIZE=8388608
        -sPTHREAD_POOL_SIZE=6
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sEXIT_RUNTIME=0
        -sENVIRONMENT=web,worker
        -sINVOKE_RUN=0
        -sMODULARIZE=0
        "SHELL:-sEXPORTED_FUNCTIONS=_main,_Rebel_Post_Message,_Rebel_Set_Screen_Size,_Rebel_Input_Key,_Rebel_Input_Mouse_Move,_Rebel_Input_Mouse_Wheel,_Rebel_Input_Mouse_Button,_Rebel_Input_Release_All,_Rebel_Audio_Update,_Rebel_Debug_Dump,_malloc,_free"
        "SHELL:-sEXPORTED_RUNTIME_METHODS=callMain,ccall,cwrap,UTF8ToString,stringToUTF8,HEAPU16"
    )
    set_target_properties(Renegade PROPERTIES SUFFIX ".js" OUTPUT_NAME "renegade" OUTPUT_NAME_DEBUG "renegade" OUTPUT_NAME_RELWITHDEBINFO "renegade")
endif()
