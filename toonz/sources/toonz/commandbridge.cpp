// CommandBridge - local JSON command interface for OpenToonz (custom fork add-on).
// See commandbridge.h for the protocol. Original add-on code; it only calls
// existing OpenToonz APIs (TApp, IoCmd, TXsheet, TStageObject, TFxCommand...).

#include "commandbridge.h"

// Toonz app
#include "tapp.h"
#include "iocommand.h"

// TnzLib
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tcamera.h"
#include "toonz/sceneproperties.h"
#include "toonz/levelset.h"
#include "toonz/txshlevel.h"
#include "toonz/txshcell.h"
#include "toonz/fxcommand.h"
#include "toutputproperties.h"

// TnzBase / TnzCore
#include "tfx.h"
#include "tparamcontainer.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tfilepath.h"

// TnzQt
#include "toonzqt/menubarcommand.h"

// Qt
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>

#include <stdexcept>

namespace {

const char *kBridgeVersion = "otz-bridge 0.1";

struct BridgeError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const QString &msg) {
  throw BridgeError(msg.toStdString());
}

QString reqString(const QJsonObject &p, const char *key) {
  if (!p.contains(key) || !p.value(key).isString())
    fail(QString("missing string parameter '%1'").arg(key));
  return p.value(key).toString();
}

int reqInt(const QJsonObject &p, const char *key) {
  if (!p.contains(key) || !p.value(key).isDouble())
    fail(QString("missing integer parameter '%1'").arg(key));
  return p.value(key).toInt();
}

double reqNum(const QJsonObject &p, const char *key) {
  if (!p.contains(key) || !p.value(key).isDouble())
    fail(QString("missing number parameter '%1'").arg(key));
  return p.value(key).toDouble();
}

TApp *app() { return TApp::instance(); }
ToonzScene *scene() { return app()->getCurrentScene()->getScene(); }
TXsheet *xsheet() { return app()->getCurrentXsheet()->getXsheet(); }

TCamera *currentCamera() {
  TCamera *cam = xsheet()->getStageObjectTree()->getCurrentCamera();
  if (!cam) fail("no current camera");
  return cam;
}

// "col:3" (1-based column, as shown in the Xsheet), "camera", "table", "peg:1"
TStageObjectId parseTarget(const QString &t) {
  if (t == "camera")
    return xsheet()->getStageObjectTree()->getCurrentCameraId();
  if (t == "table") return TStageObjectId::TableId;
  if (t.startsWith("col:")) {
    int c = t.mid(4).toInt();
    if (c < 1) fail("column targets are 1-based: col:1, col:2, ...");
    return TStageObjectId::ColumnId(c - 1);
  }
  if (t.startsWith("peg:")) {
    int c = t.mid(4).toInt();
    if (c < 1) fail("pegbar targets are 1-based: peg:1, ...");
    return TStageObjectId::PegbarId(c - 1);
  }
  fail("target must be camera | table | col:N | peg:N");
}

TStageObject::Channel parseChannel(const QString &c) {
  static const QHash<QString, TStageObject::Channel> map = {
      {"x", TStageObject::T_X},           {"y", TStageObject::T_Y},
      {"z", TStageObject::T_Z},           {"so", TStageObject::T_SO},
      {"angle", TStageObject::T_Angle},   {"scale", TStageObject::T_Scale},
      {"sx", TStageObject::T_ScaleX},     {"sy", TStageObject::T_ScaleY},
      {"shx", TStageObject::T_ShearX},    {"shy", TStageObject::T_ShearY},
      {"path", TStageObject::T_Path}};
  auto it = map.find(c);
  if (it == map.end())
    fail("channel must be x|y|z|so|angle|scale|sx|sy|shx|shy|path");
  return it.value();
}

TDoubleKeyframe::Type parseInterp(const QString &s) {
  if (s.isEmpty() || s == "linear") return TDoubleKeyframe::Linear;
  if (s == "ease") return TDoubleKeyframe::EaseInOut;
  if (s == "ease_pct") return TDoubleKeyframe::EaseInOutPercentage;
  if (s == "speed") return TDoubleKeyframe::SpeedInOut;
  if (s == "constant") return TDoubleKeyframe::Constant;
  if (s == "exp") return TDoubleKeyframe::Exponential;
  fail("interp must be linear|ease|ease_pct|speed|constant|exp");
}

TXshLevel *findLevel(const QString &name) {
  TXshLevel *lvl = scene()->getLevelSet()->getLevel(name.toStdWString());
  if (!lvl) fail(QString("level '%1' is not in the scene").arg(name));
  return lvl;
}

}  // namespace

//=============================================================================

void CommandBridge::startIfEnabled() {
  static CommandBridge *instance = nullptr;
  if (instance) return;
  const QByteArray portEnv = qgetenv("OTZ_BRIDGE_PORT");
  if (portEnv.isEmpty()) return;
  bool ok    = false;
  int port   = portEnv.toInt(&ok);
  if (!ok || port <= 0 || port > 65535) {
    qWarning("otz-bridge: invalid OTZ_BRIDGE_PORT");
    return;
  }
  instance = new CommandBridge(quint16(port),
                               QString::fromUtf8(qgetenv("OTZ_BRIDGE_TOKEN")),
                               QCoreApplication::instance());
}

CommandBridge::CommandBridge(quint16 port, const QString &token, QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)), m_token(token) {
  connect(m_server, &QTcpServer::newConnection, this,
          &CommandBridge::onNewConnection);
  if (!m_server->listen(QHostAddress::LocalHost, port))
    qWarning("otz-bridge: cannot listen on 127.0.0.1:%d: %s", int(port),
             qPrintable(m_server->errorString()));
  else
    qInfo("otz-bridge: listening on 127.0.0.1:%d", int(port));
}

void CommandBridge::onNewConnection() {
  while (QTcpSocket *s = m_server->nextPendingConnection()) {
    m_buffers.insert(s, QByteArray());
    connect(s, &QTcpSocket::readyRead, this, &CommandBridge::onReadyRead);
    connect(s, &QTcpSocket::disconnected, this, [this, s]() {
      m_buffers.remove(s);
      s->deleteLater();
    });
  }
}

void CommandBridge::onReadyRead() {
  auto *s = qobject_cast<QTcpSocket *>(sender());
  if (!s) return;
  QByteArray &buf = m_buffers[s];
  buf += s->readAll();
  if (buf.size() > 64 * 1024 * 1024) {  // refuse absurd payloads
    s->disconnectFromHost();
    return;
  }
  int nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const QByteArray line = buf.left(nl).trimmed();
    buf.remove(0, nl + 1);
    if (line.isEmpty()) continue;

    QJsonObject resp;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
      resp["ok"]    = false;
      resp["error"] = "invalid JSON: " + perr.errorString();
    } else {
      const QJsonObject req = doc.object();
      resp["id"]            = req.value("id");
      if (!m_token.isEmpty() && req.value("token").toString() != m_token) {
        resp["ok"]    = false;
        resp["error"] = "bad or missing token";
      } else {
        try {
          resp["result"] = dispatch(req.value("method").toString(),
                                    req.value("params").toObject());
          resp["ok"]     = true;
        } catch (const std::exception &e) {
          resp["ok"]    = false;
          resp["error"] = QString::fromUtf8(e.what());
        } catch (...) {
          resp["ok"]    = false;
          resp["error"] = "unknown exception inside OpenToonz";
        }
      }
    }
    s->write(QJsonDocument(resp).toJson(QJsonDocument::Compact) + "\n");
    s->flush();
  }
}

QJsonValue CommandBridge::dispatch(const QString &m, const QJsonObject &p) {
  using H = QJsonValue (CommandBridge::*)(const QJsonObject &);
  static const QHash<QString, H> table = {
      {"ping", &CommandBridge::ping},
      {"scene.new", &CommandBridge::sceneNew},
      {"scene.load", &CommandBridge::sceneLoad},
      {"scene.save", &CommandBridge::sceneSave},
      {"scene.info", &CommandBridge::sceneInfo},
      {"scene.set_camera", &CommandBridge::sceneSetCamera},
      {"level.load", &CommandBridge::levelLoad},
      {"cells.set", &CommandBridge::cellsSet},
      {"cells.clear", &CommandBridge::cellsClear},
      {"key.set", &CommandBridge::keySet},
      {"key.clear", &CommandBridge::keyClear},
      {"fx.add", &CommandBridge::fxAdd},
      {"command.run", &CommandBridge::commandRun},
      {"frame.set", &CommandBridge::frameSet},
      {"render", &CommandBridge::render},
      {"render.status", &CommandBridge::renderStatus}};
  auto it = table.find(m);
  if (it == table.end()) fail("unknown method: " + m);
  return (this->*(it.value()))(p);
}

void CommandBridge::notifyChanged() {
  app()->getCurrentScene()->setDirtyFlag(true);
  app()->getCurrentXsheet()->notifyXsheetChanged();
}

//=============================================================================
// handlers

QJsonValue CommandBridge::ping(const QJsonObject &) {
  QJsonObject r;
  r["bridge"]  = kBridgeVersion;
  r["app_dir"] = QCoreApplication::applicationDirPath();
  return r;
}

// params: {discard_changes: bool (default false)}
QJsonValue CommandBridge::sceneNew(const QJsonObject &p) {
  if (p.value("discard_changes").toBool(false))
    app()->getCurrentScene()->setDirtyFlag(false);  // avoid the save prompt
  else if (app()->getCurrentScene()->getDirtyFlag())
    fail("current scene has unsaved changes (pass discard_changes:true)");
  IoCmd::newScene();
  return sceneInfo({});
}

// params: {path}
QJsonValue CommandBridge::sceneLoad(const QJsonObject &p) {
  const TFilePath fp(reqString(p, "path"));
  if (p.value("discard_changes").toBool(false))
    app()->getCurrentScene()->setDirtyFlag(false);
  if (!IoCmd::loadScene(fp, false, true)) fail("loadScene failed");
  return sceneInfo({});
}

// params: {path?}  (absolute .tnz; omitted = save to current path)
QJsonValue CommandBridge::sceneSave(const QJsonObject &p) {
  bool ok;
  if (p.contains("path"))
    ok = IoCmd::saveScene(TFilePath(reqString(p, "path")),
                          IoCmd::SILENTLY_OVERWRITE);
  else {
    if (scene()->isUntitled()) fail("scene is untitled: pass 'path'");
    ok = IoCmd::saveScene(IoCmd::SILENTLY_OVERWRITE);
  }
  if (!ok) fail("saveScene failed");
  return sceneInfo({});
}

QJsonValue CommandBridge::sceneInfo(const QJsonObject &) {
  QJsonObject r;
  ToonzScene *sc = scene();
  TXsheet *xsh   = xsheet();
  r["path"]      = sc->isUntitled()
                       ? QString()
                       : sc->decodeFilePath(sc->getScenePath()).getQString();
  r["frame_count"]  = xsh->getFrameCount();
  r["column_count"] = xsh->getColumnCount();
  TCamera *cam      = currentCamera();
  r["camera_res"]   = QJsonArray{cam->getRes().lx, cam->getRes().ly};
  r["camera_dpi"]   = cam->getDpi().x;
  r["fps"] = sc->getProperties()->getOutputProperties()->getFrameRate();
  return r;
}

// params: {width, height, fps?}   keeps 120 dpi so 1 art pixel = 1 output pixel
QJsonValue CommandBridge::sceneSetCamera(const QJsonObject &p) {
  const int w = reqInt(p, "width"), h = reqInt(p, "height");
  if (w < 16 || h < 16 || w > 8192 || h > 8192) fail("resolution out of range");
  const double dpi = p.value("dpi").toDouble(120.0);
  TCamera *cam     = currentCamera();
  cam->setRes(TDimension(w, h));
  cam->setSize(TDimensionD(w / dpi, h / dpi));
  if (p.contains("fps"))
    scene()->getProperties()->getOutputProperties()->setFrameRate(
        reqNum(p, "fps"));
  notifyChanged();
  app()->getCurrentScene()->notifySceneChanged();
  return sceneInfo({});
}

// params: {path, col (1-based), row (1-based, default 1), expose (default true)}
// Works for image sequences ("name..png" or one file of the sequence), single
// images, .pli/.tlv levels and audio files (wav/mp3 per OpenToonz support).
QJsonValue CommandBridge::levelLoad(const QJsonObject &p) {
  IoCmd::LoadResourceArguments args;
  args.resourceDatas.push_back(
      IoCmd::LoadResourceArguments::ResourceData(TFilePath(reqString(p, "path"))));
  args.col0          = reqInt(p, "col") - 1;
  args.row0          = p.value("row").toInt(1) - 1;
  args.expose        = p.value("expose").toBool(true);
  args.importPolicy  = IoCmd::LoadResourceArguments::LOAD;  // no copy prompt
  args.renamePolicy  = IoCmd::LoadResourceArguments::RenamePolicy::NEVER;
  args.convertPolicy = IoCmd::LoadResourceArguments::ConvertPolicy::NEVER;
  if (args.col0 < 0 || args.row0 < 0) fail("col/row are 1-based");
  const int n = IoCmd::loadResources(args, false);
  if (n <= 0) fail("nothing was loaded (check the path)");
  QJsonArray names;
  for (TXshLevel *l : args.loadedLevels)
    names.append(QString::fromStdWString(l->getName()));
  QJsonObject r;
  r["levels"] = names;
  r["col0"]   = args.col0 + 1;
  r["col1"]   = args.col1 + 1;
  r["row0"]   = args.row0 + 1;
  r["row1"]   = args.row1 + 1;
  notifyChanged();
  return r;
}

// params: {col (1-based), level, row0 (1-based, default 1),
//          fids: [1, 1, 2, null, 3 ...]}   null = leave empty
QJsonValue CommandBridge::cellsSet(const QJsonObject &p) {
  const int col  = reqInt(p, "col") - 1;
  const int row0 = p.value("row0").toInt(1) - 1;
  if (col < 0 || row0 < 0) fail("col/row0 are 1-based");
  TXshLevelP lvl(findLevel(reqString(p, "level")));
  const QJsonArray fids = p.value("fids").toArray();
  TXsheet *xsh          = xsheet();
  int written           = 0;
  for (int i = 0; i < fids.size(); ++i) {
    const QJsonValue v = fids.at(i);
    if (v.isNull()) {
      xsh->setCell(row0 + i, col, TXshCell());
      continue;
    }
    if (!v.isDouble()) fail("fids must be integers or null");
    xsh->setCell(row0 + i, col, TXshCell(lvl, TFrameId(v.toInt())));
    ++written;
  }
  notifyChanged();
  QJsonObject r;
  r["written"] = written;
  return r;
}

// params: {col, row0, count}
QJsonValue CommandBridge::cellsClear(const QJsonObject &p) {
  const int col = reqInt(p, "col") - 1, row0 = reqInt(p, "row0") - 1;
  const int n   = reqInt(p, "count");
  if (col < 0 || row0 < 0 || n < 0) fail("bad range");
  for (int i = 0; i < n; ++i) xsheet()->setCell(row0 + i, col, TXshCell());
  notifyChanged();
  return QJsonObject{{"cleared", n}};
}

// params: {target:"col:N"|"camera"|"table"|"peg:N", channel, frame (1-based),
//          value, unit:"px"|"inch" (x/y only, default px), interp}
// Units: x/y px are offsets from the camera centre, +y up (OpenToonz stage
// convention); scale/sx/sy are factors (1 = 100%); angle in degrees.
QJsonValue CommandBridge::keySet(const QJsonObject &p) {
  const TStageObjectId id = parseTarget(reqString(p, "target"));
  const QString chName    = reqString(p, "channel");
  const auto ch           = parseChannel(chName);
  const int frame         = reqInt(p, "frame") - 1;
  if (frame < 0) fail("frame is 1-based");
  double value = reqNum(p, "value");
  if ((chName == "x" || chName == "y") && p.value("unit").toString("px") == "px")
    value /= currentCamera()->getDpi().x;  // pixels -> inches

  TStageObject *obj = xsheet()->getStageObject(id);
  if (!obj) fail("stage object not found");
  TDoubleParam *param = obj->getParam(ch);
  if (!param) fail("channel not available");
  if (!param->setValue(frame, value))
    fail("cannot key this frame (expression/file segment?)");

  const int k = param->getClosestKeyframe(frame);
  if (k >= 0) {
    TDoubleKeyframe kf = param->getKeyframe(k);
    kf.m_type          = parseInterp(p.value("interp").toString());
    param->setKeyframe(k, kf);
  }
  notifyChanged();
  QJsonObject r;
  r["keyframes"] = param->getKeyframeCount();
  return r;
}

// params: {target, channel}  removes all keyframes on the channel
QJsonValue CommandBridge::keyClear(const QJsonObject &p) {
  TStageObject *obj = xsheet()->getStageObject(parseTarget(reqString(p, "target")));
  if (!obj) fail("stage object not found");
  TDoubleParam *param = obj->getParam(parseChannel(reqString(p, "channel")));
  if (!param) fail("channel not available");
  param->clearKeyframes();
  notifyChanged();
  return QJsonObject{{"keyframes", 0}};
}

// params: {type:"STD_blurFx", col (1-based), row (default 1),
//          params: {"value": 10, "other": [[frame1, v1], [frame2, v2]]}}
// Numeric parameters only. Number = constant; list = keyframes (1-based frames).
QJsonValue CommandBridge::fxAdd(const QJsonObject &p) {
  const QString type = reqString(p, "type");
  TFx *fx            = TFx::create(type.toStdString());
  if (!fx) fail("unknown fx type: " + type);
  TFxP holder(fx);  // keep alive while configuring

  const QJsonObject params = p.value("params").toObject();
  for (auto it = params.begin(); it != params.end(); ++it) {
    TParam *raw = fx->getParams()->getParam(it.key().toStdString());
    auto *dp    = dynamic_cast<TDoubleParam *>(raw);
    if (!dp) fail("fx parameter not found or not numeric: " + it.key());
    if (it.value().isDouble())
      dp->setDefaultValue(it.value().toDouble());
    else if (it.value().isArray()) {
      for (const QJsonValue &kv : it.value().toArray()) {
        const QJsonArray pair = kv.toArray();
        if (pair.size() != 2) fail("keyframes must be [frame, value] pairs");
        dp->setValue(pair.at(0).toInt() - 1, pair.at(1).toDouble());
      }
    } else
      fail("fx parameter value must be a number or [[frame,value],...]");
  }
  const int col = reqInt(p, "col") - 1, row = p.value("row").toInt(1) - 1;
  TFxCommand::addFx(fx, QList<TFxP>(), app(), col, row);
  notifyChanged();
  QJsonObject r;
  r["fx_id"] = QString::fromStdWString(fx->getFxId());
  return r;
}

// params: {id:"MI_SaveScene"}  runs any registered menu command
QJsonValue CommandBridge::commandRun(const QJsonObject &p) {
  const QByteArray id = reqString(p, "id").toLatin1();
  if (!CommandManager::instance()->getAction(id.constData()))
    fail("unknown command id: " + QString::fromLatin1(id));
  CommandManager::instance()->execute(id.constData());
  return QJsonObject{{"executed", QString::fromLatin1(id)}};
}

// params: {frame (1-based)}  moves the GUI's current frame
QJsonValue CommandBridge::frameSet(const QJsonObject &p) {
  const int f = reqInt(p, "frame") - 1;
  if (f < 0) fail("frame is 1-based");
  app()->getCurrentFrame()->setFrame(f);
  return QJsonObject{{"frame", f + 1}};
}

// params: {output: "C:/out/shot_.png", r0?, r1?}  (1-based, inclusive)
// Saves the scene, then runs the bundled tcomposer asynchronously.
QJsonValue CommandBridge::render(const QJsonObject &p) {
  if (scene()->isUntitled()) fail("save the scene first (scene.save with path)");
  if (!IoCmd::saveScene(IoCmd::SILENTLY_OVERWRITE)) fail("saveScene failed");
  const QString scenePath =
      scene()->decodeFilePath(scene()->getScenePath()).getQString();
  const QString output = reqString(p, "output");
  QDir().mkpath(QFileInfo(output).absolutePath());

#ifdef _WIN32
  const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("tcomposer.exe");
#else
  const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("tcomposer");
#endif
  if (!QFileInfo::exists(exe)) fail("tcomposer not found next to OpenToonz: " + exe);

  QStringList argv{QDir::toNativeSeparators(scenePath), "-o",
                   QDir::toNativeSeparators(output)};
  const int r0 = p.value("r0").toInt(1);
  const int r1 = p.value("r1").toInt(xsheet()->getFrameCount());
  argv << "-range" << QString::number(r0) << QString::number(r1);

  auto *proc = new QProcess(this);
  proc->setProcessChannelMode(QProcess::MergedChannels);
  const int jobId = m_nextJob++;
  m_jobs.insert(jobId, Job{proc, output, QString()});
  connect(proc, &QProcess::readyRead, this, [this, jobId, proc]() {
    if (m_jobs.contains(jobId))
      m_jobs[jobId].log += QString::fromLocal8Bit(proc->readAll());
  });
  proc->start(exe, argv);
  if (!proc->waitForStarted(10000)) fail("tcomposer failed to start");
  return QJsonObject{{"job", jobId}, {"scene", scenePath}};
}

// params: {job}
QJsonValue CommandBridge::renderStatus(const QJsonObject &p) {
  const int jobId = reqInt(p, "job");
  if (!m_jobs.contains(jobId)) fail("unknown job");
  Job &j = m_jobs[jobId];
  QJsonObject r;
  const bool running = j.proc->state() != QProcess::NotRunning;
  r["running"]       = running;
  if (!running) r["exit_code"] = j.proc->exitCode();
  r["output"] = j.output;
  r["log_tail"] = j.log.right(2000);
  return r;
}
