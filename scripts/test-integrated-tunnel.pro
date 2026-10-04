QT += core network
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = tunnel-manager-tests
INCLUDEPATH += ../app ../moonlight-common-c/moonlight-common-c/src
SOURCES += test-integrated-tunnel.cpp ../app/backend/integratedtunnel.cpp
HEADERS += ../app/backend/integratedtunnel.h
