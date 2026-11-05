#pragma once
// CommandBridge - local JSON command interface for OpenToonz (custom fork add-on).
//
// Disabled unless the environment variable OTZ_BRIDGE_PORT is set (e.g. 7788).
// Listens on 127.0.0.1 only. Optional shared secret: OTZ_BRIDGE_TOKEN.
// Protocol: one JSON object per line.
//   request : {"id":1,"method":"scene.info","params":{...},"token":"..."}
//   response: {"id":1,"ok":true,"result":{...}}  or  {"id":1,"ok":false,"error":"..."}
// All handlers run on the GUI thread (QTcpServer lives in the main thread),
// so they may safely touch TApp / the current scene.

#ifndef COMMANDBRIDGE_H
#define COMMANDBRIDGE_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>

class QTcpServer;
class QTcpSocket;
class QProcess;

class CommandBridge final : public QObject {
  Q_OBJECT

public:
  // Starts the server if OTZ_BRIDGE_PORT is set. Safe to call once at startup.
  static void startIfEnabled();

private:
  explicit CommandBridge(quint16 port, const QString &token,
                         QObject *parent = nullptr);

  QJsonValue dispatch(const QString &method, const QJsonObject &p);

  // --- handlers -----------------------------------------------------------
  QJsonValue ping(const QJsonObject &p);
  QJsonValue sceneNew(const QJsonObject &p);
  QJsonValue sceneLoad(const QJsonObject &p);
  QJsonValue sceneSave(const QJsonObject &p);
  QJsonValue sceneInfo(const QJsonObject &p);
  QJsonValue sceneSetCamera(const QJsonObject &p);
  QJsonValue levelLoad(const QJsonObject &p);
  QJsonValue cellsSet(const QJsonObject &p);
  QJsonValue cellsClear(const QJsonObject &p);
  QJsonValue keySet(const QJsonObject &p);
  QJsonValue keyClear(const QJsonObject &p);
  QJsonValue fxAdd(const QJsonObject &p);
  QJsonValue commandRun(const QJsonObject &p);
  QJsonValue frameSet(const QJsonObject &p);
  QJsonValue render(const QJsonObject &p);
  QJsonValue renderStatus(const QJsonObject &p);

  void notifyChanged();

private slots:
  void onNewConnection();
  void onReadyRead();

private:
  QTcpServer *m_server;
  QString m_token;
  QHash<QTcpSocket *, QByteArray> m_buffers;

  struct Job {
    QProcess *proc;
    QString output;
    QString log;
  };
  QHash<int, Job> m_jobs;
  int m_nextJob = 1;
};

#endif  // COMMANDBRIDGE_H
