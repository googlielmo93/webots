// Copyright 1996-2022 Cyberbotics Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "WbProtoList.hpp"

#include "../app/WbApplication.hpp"
#include "../nodes/utils/WbDownloader.hpp"
#include "WbLog.hpp"
#include "WbNode.hpp"
#include "WbParser.hpp"
#include "WbPreferences.hpp"
#include "WbProtoModel.hpp"
#include "WbStandardPaths.hpp"
#include "WbTokenizer.hpp"
#include "WbVrmlWriter.hpp"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QRegularExpression>

static WbProtoList *gCurrent = NULL;
QFileInfoList WbProtoList::gResourcesProtoCache;
QFileInfoList WbProtoList::gProjectsProtoCache;
QFileInfoList WbProtoList::gExtraProtoCache;

WbProtoList *WbProtoList::current() {
  if (!gCurrent)
    gCurrent = new WbProtoList();
  return gCurrent;
}

/*
WbProtoList::WbProtoList(const QString &primarySearchPath) {
  gCurrent = this;
  mPrimarySearchPath = primarySearchPath;

  static bool firstCall = true;
  if (firstCall) {
    updateResourcesProtoCache();
    updateProjectsProtoCache();
    updateExtraProtoCache();
    firstCall = false;
  }

  updatePrimaryProtoCache();
}
*/

WbProtoList::WbProtoList() {
  setupKnownProtoList();
}

/*
WbProtoList::WbProtoList(const QString &world, bool reloading) {
  printf("WbProtoList::WbProtoList()\n");

  // create protos folder in temporary directory, it will hold all extern references
  // QDir dir();
  // if (QDir(WbStandardPaths::webotsTmpProtoPath()).exists())
  //  printf("Shouldn't be calling this if it already exists");  // TODO: check
  // else
  //  QDir().mkpath(WbStandardPaths::webotsTmpProtoPath());

  mCurrentWorld = world;
  mReloading = reloading;
  downloadExternProto(world, WbStandardPaths::webotsTmpProtoPath());
}
*/

// we do not delete the PROTO models here: each PROTO model is automatically deleted when its last PROTO instance is deleted
WbProtoList::~WbProtoList() {
  if (gCurrent == this)
    gCurrent = NULL;

  foreach (WbProtoModel *model, mModels)
    model->unref();

  clearProtoSearchPaths();
}

void WbProtoList::findProtosRecursively(const QString &dirPath, QFileInfoList &protoList, bool inProtos) {
  QDir dir(dirPath);
  if (!dir.exists() || !dir.isReadable())
    // no PROTO nodes
    return;

  // search in folder
  if (inProtos) {
    QStringList filter("*.proto");
    protoList.append(dir.entryInfoList(filter, QDir::Files, QDir::Name));
  }
  // search in subfolders
  QFileInfoList subfolderInfoList = dir.entryInfoList(QDir::AllDirs | QDir::NoSymLinks | QDir::NoDotAndDotDot);

  if (!inProtos) {
    // try to identify a project root folder
    foreach (QFileInfo subfolder, subfolderInfoList) {
      const QString &fileName = subfolder.fileName();
      if (fileName == "controllers" || fileName == "worlds" || fileName == "protos" || fileName == "plugins") {
        const QString protosPath = dirPath + "/protos";
        if (QFile::exists(protosPath))
          findProtosRecursively(protosPath, protoList, true);
        return;
      }
    }
  }
  foreach (QFileInfo subfolder, subfolderInfoList) {
    if (inProtos &&
        (subfolder.fileName() == "textures" || subfolder.fileName() == "icons" || subfolder.fileName() == "meshes")) {
      // skip any textures or icons subfolder inside a protos folder
      continue;
    }
    findProtosRecursively(subfolder.absoluteFilePath(), protoList, inProtos);
  }
}

void WbProtoList::updateResourcesProtoCache() {
  gResourcesProtoCache.clear();
  QFileInfoList protosInfo;
  findProtosRecursively(WbStandardPaths::resourcesProjectsPath(), protosInfo);
  gResourcesProtoCache << protosInfo;
}

void WbProtoList::updateProjectsProtoCache() {
  gProjectsProtoCache.clear();
  QFileInfoList protosInfo;
  findProtosRecursively(WbStandardPaths::projectsPath(), protosInfo);
  gProjectsProtoCache << protosInfo;
}

void WbProtoList::updateExtraProtoCache() {
  gExtraProtoCache.clear();
  QFileInfoList protosInfo;
  if (!WbPreferences::instance()->value("General/extraProjectsPath").toString().isEmpty())
    findProtosRecursively(WbPreferences::instance()->value("General/extraProjectsPath").toString(), protosInfo);
  gExtraProtoCache << protosInfo;
}

void WbProtoList::updatePrimaryProtoCache() {
  mPrimaryProtoCache.clear();

  if (mPrimarySearchPath.isEmpty())
    return;

  QFileInfoList protosInfo;
  findProtosRecursively(mPrimarySearchPath, protosInfo, mPrimarySearchPath.endsWith("protos"));
  mPrimaryProtoCache << protosInfo;
}

WbProtoModel *WbProtoList::readModel(const QString &fileName, const QString &worldPath, QStringList baseTypeList) const {
  printf("WbProtoList::readModel(arg string)\n");
  WbTokenizer tokenizer;
  int errors = tokenizer.tokenize(fileName);
  if (errors > 0)
    return NULL;

  // TODO: should be moved elsewhere (WbParser), as this point might be reached while parsing a world too
  WbParser parser(&tokenizer);

  while (tokenizer.peekWord() == "EXTERNPROTO")  // consume all EXTERNPROTO tokens, if any
    parser.skipExternProto();

  if (!parser.parseProtoInterface(worldPath))
    return NULL;

  printf("readmodel (next word is %s)\n", tokenizer.peekWord().toUtf8().constData());

  tokenizer.rewind();

  while (tokenizer.peekWord() == "EXTERNPROTO")  // consume all EXTERNPROTO tokens, if any
    parser.skipExternProto();

  bool prevInstantiateMode = WbNode::instantiateMode();
  try {
    WbNode::setInstantiateMode(false);
    WbProtoModel *model = new WbProtoModel(&tokenizer, worldPath, fileName, baseTypeList);
    WbNode::setInstantiateMode(prevInstantiateMode);
    return model;
  } catch (...) {
    WbNode::setInstantiateMode(prevInstantiateMode);
    return NULL;
  }
}

void WbProtoList::readModel(WbTokenizer *tokenizer, const QString &worldPath) {
  printf("WbProtoList::readModel (arg tokenizer)\n");
  WbProtoModel *model = NULL;
  bool prevInstantiateMode = WbNode::instantiateMode();
  try {
    WbNode::setInstantiateMode(false);
    model = new WbProtoModel(tokenizer, worldPath);
    WbNode::setInstantiateMode(prevInstantiateMode);
  } catch (...) {
    WbNode::setInstantiateMode(prevInstantiateMode);
    return;
  }
  mModels.prepend(model);
  model->ref();
}

WbProtoModel *WbProtoList::customFindModel(const QString &modelName, const QString &worldPath, QStringList baseTypeList) {
  printf("WbProtoList::customFindModel\n");
  // return NULL;

  foreach (WbProtoModel *model, mModels)
    if (model->name() == modelName)
      return model;

  QFileInfoList tmpProto;
  QDirIterator it(WbStandardPaths::webotsTmpProtoPath(), QStringList("*.proto"), QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    QFileInfo fi(it.next());
    tmpProto << fi;
    printf("-- found %s\n", fi.fileName().toUtf8().constData());
  }

  foreach (const QFileInfo &fi, tmpProto) {
    if (fi.baseName() == modelName) {
      WbProtoModel *model = readModel(fi.absoluteFilePath(), worldPath, baseTypeList);
      if (model == NULL)  // can occur if the PROTO contains errors
        return NULL;
      mModels << model;
      model->ref();
      return model;
    }
  }

  return NULL;
}

WbProtoModel *WbProtoList::findModel(const QString &modelName, const QString &worldPath, QStringList baseTypeList) {
  printf("WbProtoList::findModel\n");
  // see if model is already loaded
  foreach (WbProtoModel *model, mModels)
    if (model->name() == modelName)
      return model;

  QFileInfoList tmpProto;  // protos in Webots temporary folder (i.e added by EXTERNPROTO reference)
  foreach (const QString &path, WbStandardPaths::webotsTmpProtoPath())
    findProtosRecursively(path, tmpProto);  // TODO: works because folder in tmp is called "protos". No need to have list of
                                            // searchable paths for each primary proto if this is good enough
  printf("> done searching\n");

  foreach (const QFileInfo &fi, tmpProto) {
    if (fi.baseName() == modelName) {
      WbProtoModel *model = readModel(fi.absoluteFilePath(), worldPath, baseTypeList);
      if (model == NULL)  // can occur if the PROTO contains errors
        return NULL;
      mModels << model;
      model->ref();
      return model;
    }
  }

  return NULL;  // not found
}

QString WbProtoList::findModelPath(const QString &modelName) const {
  printf("WbProtoList::findModelPath\n");
  QFileInfoList availableProtoFiles;
  availableProtoFiles << mPrimaryProtoCache << gExtraProtoCache << gProjectsProtoCache << gResourcesProtoCache;

  foreach (const QFileInfo &fi, availableProtoFiles) {
    if (fi.baseName() == modelName)
      return fi.absoluteFilePath();
  }

  return QString();  // not found
}

QStringList WbProtoList::fileList() {
  QStringList list;
  foreach (WbProtoModel *model, gCurrent->mModels)
    list << model->fileName();
  return list;
}

QStringList WbProtoList::fileList(int cache) {
  QStringList list;

  QFileInfoList availableProtoFiles;
  switch (cache) {
    case RESOURCES_PROTO_CACHE:
      availableProtoFiles << gResourcesProtoCache;
      break;
    case PROJECTS_PROTO_CACHE:
      availableProtoFiles << gProjectsProtoCache;
      break;
    case EXTRA_PROTO_CACHE:
      availableProtoFiles << gExtraProtoCache;
      break;
    default:
      return list;
  }

  foreach (const QFileInfo &fi, availableProtoFiles)
    list.append(fi.baseName());

  return list;
}

void WbProtoList::clearProtoSearchPaths(void) {
  printf("> clearing proto search paths\n");
  mProtoSearchPaths.clear();
  // TODO: add current working project path by default (location of world file), others?
}

void WbProtoList::insertProtoSearchPath(const QString &path) {
  QDir dir(path);
  if (dir.exists() && !mProtoSearchPaths.contains(path))
    mProtoSearchPaths << path;

  printf("Searchable paths:\n");
  foreach (const QString &path, mProtoSearchPaths)
    printf("- %s\n", path.toUtf8().constData());
}

bool WbProtoList::areProtoAssetsAvailable(const QString &filename) {
  QMap<QString, QString> externProtos = getExternProtoList(filename);

  QMapIterator<QString, QString> it(externProtos);
  while (it.hasNext()) {  // TODO: need to check full depth or just at world level?
    it.next();
    const QString path = WbStandardPaths::webotsTmpProtoPath() + it.key() + "/" + it.key() + ".proto";
    if (!QFileInfo(path).exists()) {
      printf(" > proto assets not available, begin download\n");
      return false;
    }
  }

  printf(" > proto assets available, world can be parsed\n");
  return true;
}

void WbProtoList::retrieveExternProto(QString filename, bool reloading) {
  QDir temporaryProtoDir(WbStandardPaths::webotsTmpProtoPath());
  if (temporaryProtoDir.exists())
    temporaryProtoDir.removeRecursively();
  QDir().mkpath(WbStandardPaths::webotsTmpProtoPath());

  mCurrentWorld = filename;
  mReloading = reloading;
  connect(this, &WbProtoList::protoRetrieved, this, &WbProtoList::retrievalCompletionTracker);
  recursiveProtoRetrieval(filename, temporaryProtoDir.absolutePath());
}

QMap<QString, QString> WbProtoList::getExternProtoList(const QString &filename) {
  QMap<QString, QString> protoList;

  QFile file(filename);
  if (file.open(QIODevice::ReadOnly)) {
    const QString content = file.readAll();

    QRegularExpression re("EXTERNPROTO\\s([a-zA-Z0-9-_+]+)\\s\"(.*\\.proto)\"");  // TODO: test it more

    QRegularExpressionMatchIterator it = re.globalMatch(content);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      if (match.hasMatch()) {
        const QString identifier = match.captured(1);
        const QString url = match.captured(2);

        if (!url.endsWith(identifier + ".proto")) {
          WbLog::error(tr("Malformed extern proto url. The identifier and url do not coincide.\n"));
          return protoList;
        }

        protoList.insert(identifier, url);  // if same identifier, only last url is kept

        printf(" > found |%s| |%s|\n", identifier.toUtf8().constData(), url.toUtf8().constData());
      }
    }
  }

  return protoList;
}

void WbProtoList::recursiveProtoRetrieval(const QString &filename, const QString &parent) {
  printf("recursing: %s, parent >%s<\n", filename.toUtf8().constData(), parent.toUtf8().constData());
  QMap<QString, QString> externProtos = getExternProtoList(filename);

  if (externProtos.isEmpty()) {
    WbApplication::instance()->loadWorld(mCurrentWorld, mReloading);
    return;
  }

  QMapIterator<QString, QString> it(externProtos);
  while (it.hasNext()) {
    it.next();

    // create folder using the identifier of the proto (each proto is in its own folder, sub-PROTO are sub-folders)
    QString rootPath = parent;
    if (!parent.endsWith("/"))
      rootPath += "/";
    rootPath += it.key() + "/";

    QFileInfo protoFile(rootPath + it.key() + ".proto");
    QDir dir;
    dir.mkpath(protoFile.absolutePath());
    printf(" > making dir %s\n", protoFile.absolutePath().toUtf8().constData());

    // download
    printf(" > downloading: %s to %s\n", it.key().toUtf8().constData(), protoFile.absoluteFilePath().toUtf8().constData());
    mRetrievers.push_back(new WbDownloader(this));
    mRetrievers.last()->download(QUrl(it.value()), protoFile.absoluteFilePath());
    connect(mRetrievers.last(), &WbDownloader::complete, this, &WbProtoList::recurser);
  }
}

void WbProtoList::recurser() {
  WbDownloader *retriever = dynamic_cast<WbDownloader *>(sender());
  printf(" > download complete.\n");
  if (retriever) {
    const QString parent = QFileInfo(retriever->mDestination).absolutePath();
    recursiveProtoRetrieval(retriever->mDestination, parent);
    emit protoRetrieved();
  }
}

void WbProtoList::retrievalCompletionTracker() {
  bool finished = true;
  for (int i = 0; i < mRetrievers.size(); ++i)
    if (!mRetrievers[i]->hasFinished())
      finished = false;

  if (finished) {
    printf("FINISHED, files downloaded: %d\n", mRetrievers.size());
    disconnect(this, &WbProtoList::protoRetrieved, this, &WbProtoList::retrievalCompletionTracker);
    qDeleteAll(mRetrievers);
    mRetrievers.clear();
    // load the world again
    WbApplication::instance()->loadWorld(mCurrentWorld, mReloading);
  }
}

void WbProtoList::setupKnownProtoList() {
  QDir searchPath("/home/daniel/webots_develop/projects/samples/devices/worlds");

  if (!searchPath.exists() || !searchPath.isReadable())
    printf("> path doesn't exist");  // TODO: handle this case

  QFileInfoList worlds;
  QStringList filter("*.wbt");
  worlds.append(searchPath.entryInfoList(filter, QDir::Files, QDir::Name));

  foreach (const QFileInfo &world, worlds) {
    // printf("~~> %s\n", world.fileName().toUtf8().constData());
    QMap<QString, QString> externProtos = getExternProtoList(world.fileName());
    mProtoList.insert(externProtos);

    QMapIterator<QString, QString> it(externProtos);
    while (it.hasNext()) {
      it.next();

      printf("%30s %s\n", it.key().toUtf8().constData(), it.value().toUtf8().constData());
    }
  }
}