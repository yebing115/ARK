/*
* This source file is part of ArkGameFrame
* For the latest info, see https://github.com/ArkGame
*
* Copyright (c) AFHttpEntity ArkGame authors.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
*/

#include "AFCNetServer.h"
#include <string.h>

#if ARK_PLATFORM == PLATFORM_WIN
#include <WS2tcpip.h>
#include <winsock2.h>
#pragma  comment(lib,"Ws2_32.lib")
#elif ARK_PLATFORM == PLATFORM_APPLE
#include <arpa/inet.h>
#endif

#include <atomic>
#include <memory>

void AFCNetServer::Update()
{
    ProcessMsgLogicThread();
}

int AFCNetServer::Start(const unsigned int nMaxClient, const std::string& strAddrPort, const int nServerID, const int nThreadCount)
{
    std::string strHost;
    int port;
    SplitHostPort(strAddrPort, strHost, port);
    m_plistenThread->startListen(false, strHost, port, std::bind(&AFCNetServer::OnAcceptConnectionInner, this, std::placeholders::_1));

    m_pServer->startWorkThread(nThreadCount);
    return 0;
}


size_t AFCNetServer::OnMessageInner(const brynet::net::TCPSession::PTR& session, const char* buffer, size_t len)
{
    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(session->getUD());
    AFGUID xClient(0, *ud);

    AFTCPEntity *pEntity = nullptr;

    {
        AFScopeRdLock xGuard(mRWLock);

        auto xFind = mmObject.find(xClient);
        if(xFind != mmObject.end())
        {
            pEntity = xFind->second;
        }
    }

    if (nullptr != pEntity)
    {
        pEntity->AddBuff(buffer, len);
        DismantleNet(pEntity);
    }

    return len;
}

void AFCNetServer::OnAcceptConnectionInner(brynet::net::TcpSocket::PTR socket)
{
    socket->SocketNodelay();
    m_pServer->addSession(std::move(socket),
                          brynet::net::AddSessionOption::WithEnterCallback(std::bind(&AFCNetServer::OnClientConnectionInner, this, std::placeholders::_1)),
                          brynet::net::AddSessionOption::WithMaxRecvBufferSize(1024 * 1024));
}

void AFCNetServer::OnClientConnectionInner(const brynet::net::TCPSession::PTR & session)
{
    session->setDataCallback(std::bind(&AFCNetServer::OnMessageInner, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    session->setDisConnectCallback(std::bind(&AFCNetServer::OnClientDisConnectionInner, this, std::placeholders::_1));

    AFTCPMsg* pMsg = new AFTCPMsg(session);
    pMsg->xClientID.nLow = mnNextID++;
    session->setUD(static_cast<int64_t>(pMsg->xClientID.nLow));
    pMsg->nType = CONNECTED;

    AFTCPEntity* pEntity = new AFTCPEntity(this, pMsg->xClientID, session);
    bool ret = false;

    {
        AFScopeWrLock xGuard(mRWLock);
        ret = AddNetEntity(pMsg->xClientID, pEntity);
    }

    if(ret)
    {
        pEntity->mxNetMsgMQ.Push(pMsg);
    }
    else
    {
        delete pEntity;
    }
}

void AFCNetServer::OnClientDisConnectionInner(const brynet::net::TCPSession::PTR & session)
{
    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(session->getUD());
    AFGUID xClient(0, *ud);
    AFTCPEntity *pEntity = nullptr;

    {
        AFScopeWrLock xGuard(mRWLock);

        auto xFind = mmObject.find(xClient);
        if(xFind != mmObject.end())
        {
            pEntity = xFind->second;
        }
    }

    if (nullptr != pEntity)
    {
        AFTCPMsg* pMsg = new AFTCPMsg(session);
        pMsg->xClientID = xClient;
        pMsg->nType = DISCONNECTED;

        pEntity->mxNetMsgMQ.Push(pMsg);
    }
}

void AFCNetServer::ProcessMsgLogicThread()
{
    std::list<AFGUID> xNeedRemoveList;
    {
        AFScopeRdLock xGuard(mRWLock);
        for(std::map<AFGUID, AFTCPEntity*>::iterator iter = mmObject.begin(); iter != mmObject.end(); ++iter)
        {
            ProcessMsgLogicThread(iter->second);
            if(!iter->second->NeedRemove())
            {
                continue;
            }

            xNeedRemoveList.push_back(iter->second->GetClientID());
        }
    }

    for(std::list<AFGUID>::iterator iter = xNeedRemoveList.begin(); iter != xNeedRemoveList.end(); ++iter)
    {
        AFScopeWrLock xGuard(mRWLock);
        RemoveNetEntity(*iter);
    }
}

void AFCNetServer::ProcessMsgLogicThread(AFTCPEntity* pEntity)
{
    //Handle Msg
    size_t nReceiveCount = pEntity->mxNetMsgMQ.Count();
    for(size_t i = 0; i < nReceiveCount; ++i)
    {
        AFTCPMsg* pMsg(nullptr);
        if(!pEntity->mxNetMsgMQ.Pop(pMsg))
        {
            break;
        }

        if(pMsg == nullptr)
        {
            continue;
        }

        switch(pMsg->nType)
        {
        case RECIVEDATA:
            {
                int nRet = 0;
                if(mRecvCB)
                {
                    mRecvCB(pMsg->xHead, pMsg->xHead.GetMsgID(), pMsg->strMsg.c_str(), pMsg->strMsg.size(), pEntity->GetClientID());
                }
            }
            break;
        case CONNECTED:
            {
                mEventCB((NetEventType)pMsg->nType, pMsg->xClientID, mnServerID);
            }
            break;
        case DISCONNECTED:
            {
                mEventCB((NetEventType)pMsg->nType, pMsg->xClientID, mnServerID);
                pEntity->SetNeedRemove(true);
            }
            break;
        default:
            break;
        }

        delete pMsg;
    }
}

bool AFCNetServer::Final()
{
    bWorking = false;
    return true;
}

bool AFCNetServer::SendMsgToAllClient(const char* msg, const size_t nLen)
{
    std::map<AFGUID, AFTCPEntity*>::iterator it = mmObject.begin();
    for(; it != mmObject.end(); ++it)
    {
        AFTCPEntity* pNetObject = (AFTCPEntity*)it->second;
        if(pNetObject && !pNetObject->NeedRemove())
        {
            pNetObject->GetSession()->send(msg, nLen);
        }
    }

    return true;
}

bool AFCNetServer::SendMsg(const char* msg, const size_t nLen, const AFGUID& xClient)
{
    AFScopeRdLock xGuard(mRWLock);

    AFTCPEntity* pNetObject = GetNetEntity(xClient);
    if(pNetObject == nullptr)
    {
        return false;
    }

    pNetObject->GetSession()->send(msg, nLen);
    return true;
}

bool AFCNetServer::AddNetEntity(const AFGUID& xClientID, AFTCPEntity* pEntity)
{
    return mmObject.insert(std::make_pair(xClientID, pEntity)).second;
}

bool AFCNetServer::RemoveNetEntity(const AFGUID& xClientID)
{
    AFTCPEntity* pNetObject = GetNetEntity(xClientID);
    if(pNetObject)
    {
        delete pNetObject;
        pNetObject = nullptr;
    }

    return mmObject.erase(xClientID);
}

bool AFCNetServer::CloseNetEntity(const AFGUID& xClientID)
{
    AFTCPEntity* pEntity = GetNetEntity(xClientID);
    if(pEntity)
    {
        pEntity->GetSession()->postDisConnect();
    }

    return true;
}

bool AFCNetServer::DismantleNet(AFTCPEntity* pEntity)
{
    for(; pEntity->GetBuffLen() >= AFIMsgHead::ARK_MSG_HEAD_LENGTH;)
    {
        AFCMsgHead xHead;
        int nMsgBodyLength = DeCode(pEntity->GetBuff(), pEntity->GetBuffLen(), xHead);
        if(nMsgBodyLength >= 0 && xHead.GetMsgID() > 0)
        {
            AFTCPMsg* pNetInfo = new AFTCPMsg(pEntity->GetSession());
            pNetInfo->xHead = xHead;
            pNetInfo->nType = RECIVEDATA;
            pNetInfo->strMsg.append(pEntity->GetBuff() + AFIMsgHead::ARK_MSG_HEAD_LENGTH, nMsgBodyLength);
            pEntity->mxNetMsgMQ.Push(pNetInfo);
            size_t nRet = pEntity->RemoveBuff(nMsgBodyLength + AFIMsgHead::ARK_MSG_HEAD_LENGTH);
        }
        else
        {
            break;
        }
    }

    return true;
}

bool AFCNetServer::CloseSocketAll()
{
    std::map<AFGUID, AFTCPEntity*>::iterator it = mmObject.begin();
    for(it; it != mmObject.end(); ++it)
    {
        it->second->GetSession()->postDisConnect();
        delete it->second;
        it->second = nullptr;
    }

    mmObject.clear();
    return true;
}

AFTCPEntity* AFCNetServer::GetNetEntity(const AFGUID& xClientID)
{
    std::map<AFGUID, AFTCPEntity*>::iterator it = mmObject.find(xClientID);
    if(it != mmObject.end())
    {
        return it->second;
    }

    return nullptr;
}

bool AFCNetServer::SendMsgWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xClientID, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);
    xHead.SetBodyLength(nLen);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsg(strOutData.c_str(), strOutData.length(), xClientID);
    }

    return false;
}

bool AFCNetServer::SendMsgToAllClientWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsgToAllClient(strOutData.c_str(), strOutData.length());
    }

    return false;
}

int AFCNetServer::EnCode(const AFCMsgHead& xHead, const char* strData, const size_t len, std::string& strOutData)
{
    char szHead[AFIMsgHead::ARK_MSG_HEAD_LENGTH] = { 0 };
    int nRet = xHead.EnCode(szHead);

    strOutData.clear();
    strOutData.append(szHead, AFIMsgHead::ARK_MSG_HEAD_LENGTH);
    strOutData.append(strData, len);

    return xHead.GetBodyLength() + AFIMsgHead::ARK_MSG_HEAD_LENGTH;
}

int AFCNetServer::DeCode(const char* strData, const size_t len, AFCMsgHead& xHead)
{
    if(len < AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return -1;
    }

    if(AFIMsgHead::ARK_MSG_HEAD_LENGTH != xHead.DeCode(strData))
    {
        return -2;
    }

    if(xHead.GetBodyLength() > (len - AFIMsgHead::ARK_MSG_HEAD_LENGTH))
    {
        return -3;
    }

    return xHead.GetBodyLength();
}
