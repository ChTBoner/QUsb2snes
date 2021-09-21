/*
 * Copyright (c) 2018 Sylvain "Skarsnik" Colinet.
 *
 * This file is part of the QUsb2Snes project.
 * (see https://github.com/Skarsnik/QUsb2snes).
 *
 * QUsb2Snes is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * QUsb2Snes is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with QUsb2Snes.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <QLoggingCategory>
#include <QTcpSocket>
#include <QTime>
#include <QTimer>

#include "emunetworkaccessfactory.h"


Q_LOGGING_CATEGORY(log_emunwafactory, "Emu NWA Factory")
#define sDebug() qCDebug(log_emunwafactory)


// Placeholder
const quint16 emuNetworkAccessStartPort = 65400;

EmuNetworkAccessFactory::EmuNetworkAccessFactory()
{

}


QStringList EmuNetworkAccessFactory::listDevices()
{
    return QStringList();
}

ADevice *EmuNetworkAccessFactory::attach(QString deviceName)
{
    for(auto& ci : clientInfos)
    {
        if (ci.deviceName == deviceName)
        {
            if (ci.device == nullptr)
            {
                ci.device = new EmuNetworkAccessDevice(deviceName);
            }
            if (ci.device->state() == ADevice::BUSY)
                return ci.device;
            sDebug() << "Attach, Checking stuff";
            ci.client->cmdEmuStatus();
            ci.client->waitForReadyRead(100);
            auto rep = ci.client->readReply();
            ADevice* toret = nullptr;
            if (rep.isValid)
            {
                if (rep["state"] == "no_game")
                {
                    toret = ci.device;
                } else {
                    ci.client->cmdCoreCurrentInfo();
                    ci.client->waitForReadyRead(100);
                    rep = ci.client->readReply();
                    if (rep.isValid && rep["platform"].toUpper() == "SNES")
                    {
                       toret = ci.device;
                    } else {
                        ci.lastError = "Emulator is running a no SNES game";
                        toret = nullptr;
                    }
                }
            } else {
                toret = nullptr;
            }
            return toret;
        }
    }
    return nullptr;
}

bool EmuNetworkAccessFactory::deleteDevice(ADevice *device)
{
    sDebug() << "request for deleting device";
    return false;
    QMutableListIterator<ClientInfo> it(clientInfos);
    while (it.hasNext())
    {
        it.next();
        if (it.value().device == device)
        {
            device->deleteLater();
            it.value().client->deleteLater();
            it.remove();
            return false;
        }
    }
    return false;
}

QString EmuNetworkAccessFactory::status()
{
    return QString();
}

QString EmuNetworkAccessFactory::name() const
{
    return "EmuNetworkAccess";
}


bool EmuNetworkAccessFactory::hasAsyncListDevices()
{
    return true;
}

bool EmuNetworkAccessFactory::asyncListDevices()
{
    sDebug() << "Device list";

    if (!clientInfos.isEmpty())
    {
        QTimer::singleShot(0, this, [=] {
            for (auto ci : clientInfos)
            {
                emit newDeviceName(ci.deviceName);
            }
            emit devicesListDone();
        });
        return true;
    }
    EmuNWAccessClient* client = new EmuNWAccessClient(this);
    m_state = BUSYDEVICELIST;
    QTimer::singleShot(5000, this, [=] {
        if (m_state == BUSYDEVICELIST)
        {
            sDebug() << "Device list timer timeout the request";
            client->deleteLater();
            emit devicesListDone();
            m_state = NONE;
        }
    });
    QTimer::singleShot(200, this, [=] {
        if (m_state == BUSYDEVICELIST && !client->isConnected())
        {
            sDebug() << "Connection timeout triggered";
            client->deleteLater();
            emit devicesListDone();
            m_state = NONE;
        }
    });
    sDebug() << "Trying localhost:" << emuNetworkAccessStartPort << QTime::currentTime();
    connect(client, &EmuNWAccessClient::connectError, this, [=] {
        sDebug() << "Connection error " << client->error() << QTime::currentTime();
        client->deleteLater();
        emit devicesListDone();
        m_state = NONE;
    });
    client->connectToHost("127.0.0.1", emuNetworkAccessStartPort);
    connect(client, &EmuNWAccessClient::connected, this, [=] {
        sDebug()  << "Connected" << QTime::currentTime();
        client->cmdEmuInfo();
        connect(client, &EmuNWAccessClient::readyRead, this, [=]
        {
            disconnect(client, &EmuNWAccessClient::readyRead, this, nullptr);
            auto rep = client->readReply();
            auto emuInfo = rep.toMap();
            sDebug() << emuInfo;
            client->cmdCoresList("SNES");
            QString deviceName;
            if (emuInfo.contains("id"))
                deviceName = QString("%1 - %2").arg(emuInfo["name"], emuInfo["id"]);
            else
                deviceName =  QString("%1 - %2").arg(emuInfo["name"], emuInfo["version"]);
            connect(client, &EmuNWAccessClient::readyRead, this, [=]
            {
                disconnect(client, &EmuNWAccessClient::readyRead, this, nullptr);
                auto rep = client->readReply();
                if (rep.isValid && rep.isAscii)
                {
                    ClientInfo newInfo;
                    newInfo.client = client;
                    newInfo.device = nullptr;
                    newInfo.deviceName = deviceName;
                    clientInfos.append(newInfo);
                    connect(client, &EmuNWAccessClient::disconnected, this, &EmuNetworkAccessFactory::onClientDisconnected);
                    emit newDeviceName(deviceName);
                } else {
                    client->deleteLater();
                }
                m_state = NONE;
                emit devicesListDone();
            }, Qt::UniqueConnection);
        }, Qt::UniqueConnection);
    });
    return true;
}

bool EmuNetworkAccessFactory::devicesStatus()
{
    return false;
}

void EmuNetworkAccessFactory::onClientDisconnected()
{
    sDebug() << "Client disconnected";
    //return ; // this is kinda useless?
    EmuNWAccessClient* client = qobject_cast<EmuNWAccessClient*>(sender());
    QMutableListIterator<ClientInfo> it(clientInfos);
    while (it.hasNext())
    {
        it.next();
        if (it.value().client == client)
        {
            sDebug()  << "Client disconnected, closing " << it.value().deviceName;
            if (it.value().device != nullptr)
                it.value().device->close();
            client->deleteLater();
            it.remove();
            return;
        }
    }
}
