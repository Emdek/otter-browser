/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2010 - 2014 David Rosca <nowrep@gmail.com>
* Copyright (C) 2014 - 2017 Jan Bajer aka bajasoft <jbajer@gmail.com>
* Copyright (C) 2015 - 2020 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "AdblockContentFiltersProfile.h"
#include "Console.h"
#include "Job.h"
#include "SessionsManager.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QSaveFile>
#include <QtCore/QTextStream>

namespace Otter
{

QVector<QChar> AdblockContentFiltersProfile::m_separators({QLatin1Char('_'), QLatin1Char('-'), QLatin1Char('.'), QLatin1Char('%')});
QHash<QString, AdblockContentFiltersProfile::RuleOption> AdblockContentFiltersProfile::m_options({{QLatin1String("third-party"), ThirdPartyOption}, {QLatin1String("stylesheet"), StyleSheetOption}, {QLatin1String("image"), ImageOption}, {QLatin1String("script"), ScriptOption}, {QLatin1String("object"), ObjectOption}, {QLatin1String("object-subrequest"), ObjectSubRequestOption}, {QLatin1String("object_subrequest"), ObjectSubRequestOption}, {QLatin1String("subdocument"), SubDocumentOption}, {QLatin1String("xmlhttprequest"), XmlHttpRequestOption}, {QLatin1String("websocket"), WebSocketOption}, {QLatin1String("popup"), PopupOption}, {QLatin1String("elemhide"), ElementHideOption}, {QLatin1String("generichide"), GenericHideOption}});
QHash<NetworkManager::ResourceType, AdblockContentFiltersProfile::RuleOption> AdblockContentFiltersProfile::m_resourceTypes({{NetworkManager::ImageType, ImageOption}, {NetworkManager::ScriptType, ScriptOption}, {NetworkManager::StyleSheetType, StyleSheetOption}, {NetworkManager::ObjectType, ObjectOption}, {NetworkManager::XmlHttpRequestType, XmlHttpRequestOption}, {NetworkManager::SubFrameType, SubDocumentOption},{NetworkManager::PopupType, PopupOption}, {NetworkManager::ObjectSubrequestType, ObjectSubRequestOption}, {NetworkManager::WebSocketType, WebSocketOption}});

AdblockContentFiltersProfile::AdblockContentFiltersProfile(const QString &name, const QString &title, const QUrl &updateUrl, const QDateTime &lastUpdate, const QStringList &languages, int updateInterval, ProfileCategory category, ProfileFlags flags, QObject *parent) : ContentFiltersProfile(parent),
	m_root(nullptr),
	m_dataFetchJob(nullptr),
	m_name(name),
	m_title(title),
	m_updateUrl(updateUrl),
	m_lastUpdate(lastUpdate),
	m_category(category),
	m_error(NoError),
	m_flags(flags),
	m_updateInterval(updateInterval),
	m_isEmpty(true),
	m_wasLoaded(false)
{
	if (languages.isEmpty())
	{
		m_languages = {QLocale::AnyLanguage};
	}
	else
	{
		m_languages.reserve(languages.count());

		for (int i = 0; i < languages.count(); ++i)
		{
			m_languages.append(QLocale(languages.at(i)).language());
		}
	}

	loadHeader();
}

void AdblockContentFiltersProfile::clear()
{
	if (!m_wasLoaded)
	{
		return;
	}

	if (m_root)
	{
		QtConcurrent::run(this, &AdblockContentFiltersProfile::deleteNode, m_root);
	}

	m_cosmeticFiltersRules.clear();
	m_cosmeticFiltersDomainExceptions.clear();
	m_cosmeticFiltersDomainRules.clear();

	m_wasLoaded = false;
}

void AdblockContentFiltersProfile::loadHeader()
{
	const HeaderInformation information(loadHeader(getPath()));

	if (information.error != NoError)
	{
		raiseError(information.errorString, information.error);

		return;
	}

	if (!m_flags.testFlag(HasCustomTitleFlag) && !information.title.isEmpty())
	{
		m_title = information.title;
	}

	m_isEmpty = information.isEmpty;

	if (!m_dataFetchJob && m_updateInterval > 0 && (!m_lastUpdate.isValid() || m_lastUpdate.daysTo(QDateTime::currentDateTimeUtc()) > m_updateInterval))
	{
		update();
	}
}

AdblockContentFiltersProfile::HeaderInformation AdblockContentFiltersProfile::loadHeader(const QString &path)
{
	HeaderInformation information;

	if (!QFile::exists(path))
	{
		return information;
	}

	QFile file(path);

	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		information.errorString = QCoreApplication::translate("main", "Failed to open content blocking profile file: %1").arg(file.errorString());
		information.error = ReadError;

		return information;
	}

	QTextStream stream(&file);
	stream.setCodec("UTF-8");

	const QString header(stream.readLine());

	if (!header.contains(QLatin1String("[Adblock"), Qt::CaseInsensitive))
	{
		information.errorString = QCoreApplication::translate("main", "Failed to update content blocking profile: invalid header");
		information.error = ParseError;

		return information;
	}

	int lineNumber(1);

	while (!stream.atEnd())
	{
		QString line(stream.readLine().trimmed());

		if (information.isEmpty && !line.isEmpty() && !line.startsWith(QLatin1Char('!')))
		{
			information.isEmpty = false;
		}

		if (line.startsWith(QLatin1String("! Title: ")))
		{
			information.title = line.remove(QLatin1String("! Title: ")).trimmed();

			continue;
		}

		if (lineNumber > 50)
		{
			break;
		}

		++lineNumber;
	}

	file.close();

	return information;
}

void AdblockContentFiltersProfile::parseRuleLine(const QString &rule)
{
	if (rule.isEmpty() || rule.indexOf(QLatin1Char('!')) == 0)
	{
		return;
	}

	if (rule.startsWith(QLatin1String("##")))
	{
		if (ContentFiltersManager::getCosmeticFiltersMode() == ContentFiltersManager::AllFilters)
		{
			m_cosmeticFiltersRules.append(rule.mid(2));
		}

		return;
	}

	if (rule.contains(QLatin1String("##")))
	{
		if (ContentFiltersManager::getCosmeticFiltersMode() != ContentFiltersManager::NoFilters)
		{
			parseStyleSheetRule(rule.split(QLatin1String("##")), m_cosmeticFiltersDomainRules);
		}

		return;
	}

	if (rule.contains(QLatin1String("#@#")))
	{
		if (ContentFiltersManager::getCosmeticFiltersMode() != ContentFiltersManager::NoFilters)
		{
			parseStyleSheetRule(rule.split(QLatin1String("#@#")), m_cosmeticFiltersDomainExceptions);
		}

		return;
	}

	const int optionsSeparator(rule.indexOf(QLatin1Char('$')));
	const QStringList options((optionsSeparator >= 0) ? rule.mid(optionsSeparator + 1).split(QLatin1Char(','), QString::SkipEmptyParts) : QStringList());
	QString line(rule);

	if (optionsSeparator >= 0)
	{
		line = line.left(optionsSeparator);
	}

	if (line.endsWith(QLatin1Char('*')))
	{
		line = line.left(line.length() - 1);
	}

	if (line.startsWith(QLatin1Char('*')))
	{
		line = line.mid(1);
	}

	if (!ContentFiltersManager::areWildcardsEnabled() && line.contains(QLatin1Char('*')))
	{
		return;
	}

	Node::Rule *definition(new Node::Rule());
	definition->rule = rule;
	definition->isException = line.startsWith(QLatin1String("@@"));

	if (definition->isException)
	{
		line = line.mid(2);
	}

	definition->needsDomainCheck = line.startsWith(QLatin1String("||"));

	if (definition->needsDomainCheck)
	{
		line = line.mid(2);
	}

	if (line.startsWith(QLatin1Char('|')))
	{
		definition->ruleMatch = StartMatch;

		line = line.mid(1);
	}

	if (line.endsWith(QLatin1Char('|')))
	{
		definition->ruleMatch = ((definition->ruleMatch == StartMatch) ? ExactMatch : EndMatch);

		line = line.left(line.length() - 1);
	}

	for (int i = 0; i < options.count(); ++i)
	{
		const bool optionException(options.at(i).startsWith(QLatin1Char('~')));
		const QString optionName(optionException ? options.at(i).mid(1) : options.at(i));

		if (m_options.contains(optionName))
		{
			const RuleOption option(m_options.value(optionName));

			if ((!definition->isException || optionException) && (option == ElementHideOption || option == GenericHideOption))
			{
				continue;
			}

			if (!optionException)
			{
				definition->ruleOptions |= option;
			}
			else if (option != WebSocketOption && option != PopupOption)
			{
				definition->ruleExceptions |= option;
			}
		}
		else if (optionName.startsWith(QLatin1String("domain")))
		{
			const QStringList parsedDomains(options.at(i).mid(options.at(i).indexOf(QLatin1Char('=')) + 1).split(QLatin1Char('|'), QString::SkipEmptyParts));

			for (int j = 0; j < parsedDomains.count(); ++j)
			{
				if (parsedDomains.at(j).startsWith(QLatin1Char('~')))
				{
					definition->allowedDomains.append(parsedDomains.at(j).mid(1));

					continue;
				}

				definition->blockedDomains.append(parsedDomains.at(j));
			}
		}
		else
		{
			return;
		}
	}

	Node *node(m_root);

	for (int i = 0; i < line.length(); ++i)
	{
		const QChar value(line.at(i));
		bool childrenExists(false);

		for (int j = 0; j < node->children.count(); ++j)
		{
			Node *nextNode(node->children.at(j));

			if (nextNode->value == value)
			{
				node = nextNode;

				childrenExists = true;

				break;
			}
		}

		if (!childrenExists)
		{
			Node *newNode(new Node());
			newNode->value = value;

			if (value == QLatin1Char('^'))
			{
				node->children.insert(0, newNode);
			}
			else
			{
				node->children.append(newNode);
			}

			node = newNode;
		}
	}

	node->rules.append(definition);
}

void AdblockContentFiltersProfile::parseStyleSheetRule(const QStringList &line, QMultiHash<QString, QString> &list) const
{
	const QStringList domains(line.at(0).split(QLatin1Char(',')));

	for (int i = 0; i < domains.count(); ++i)
	{
		list.insert(domains.at(i), line.at(1));
	}
}

void AdblockContentFiltersProfile::deleteNode(Node *node) const
{
	for (int i = 0; i < node->children.count(); ++i)
	{
		deleteNode(node->children.at(i));
	}

	for (int i = 0; i < node->rules.count(); ++i)
	{
		delete node->rules.at(i);
	}

	delete node;
}

ContentFiltersManager::CheckResult AdblockContentFiltersProfile::checkUrlSubstring(const Node *node, const QString &subString, QString currentRule, const Request &request) const
{
	ContentFiltersManager::CheckResult result;
	ContentFiltersManager::CheckResult currentResult;

	for (int i = 0; i < subString.length(); ++i)
	{
		const QChar treeChar(subString.at(i));
		bool childrenExists(false);

		currentResult = evaluateNodeRules(node, currentRule, request);

		if (currentResult.isBlocked)
		{
			result = currentResult;
		}
		else if (currentResult.isException)
		{
			return currentResult;
		}

		for (int j = 0; j < node->children.count(); ++j)
		{
			const Node *nextNode(node->children.at(j));

			if (nextNode->value == QLatin1Char('*'))
			{
				const QString wildcardSubString(subString.mid(i));

				for (int k = 0; k < wildcardSubString.length(); ++k)
				{
					currentResult = checkUrlSubstring(nextNode, wildcardSubString.right(wildcardSubString.length() - k), (currentRule + wildcardSubString.left(k)), request);

					if (currentResult.isBlocked)
					{
						result = currentResult;
					}
					else if (currentResult.isException)
					{
						return currentResult;
					}
				}
			}

			if (nextNode->value == QLatin1Char('^') && !treeChar.isDigit() && !treeChar.isLetter() && !m_separators.contains(treeChar))
			{
				currentResult = checkUrlSubstring(nextNode, subString.mid(i), currentRule, request);

				if (currentResult.isBlocked)
				{
					result = currentResult;
				}
				else if (currentResult.isException)
				{
					return currentResult;
				}
			}

			if (nextNode->value == treeChar)
			{
				node = nextNode;

				childrenExists = true;

				break;
			}
		}

		if (!childrenExists)
		{
			return result;
		}

		currentRule += treeChar;
	}

	currentResult = evaluateNodeRules(node, currentRule, request);

	if (currentResult.isBlocked)
	{
		result = currentResult;
	}
	else if (currentResult.isException)
	{
		return currentResult;
	}

	for (int i = 0; i < node->children.count(); ++i)
	{
		if (node->children.at(i)->value == QLatin1Char('^'))
		{
			currentResult = evaluateNodeRules(node, currentRule, request);

			if (currentResult.isBlocked)
			{
				result = currentResult;
			}
			else if (currentResult.isException)
			{
				return currentResult;
			}
		}
	}

	return result;
}

ContentFiltersManager::CheckResult AdblockContentFiltersProfile::checkRuleMatch(const Node::Rule *rule, const QString &currentRule, const Request &request) const
{
	switch (rule->ruleMatch)
	{
		case StartMatch:
			if (!request.requestUrl.startsWith(currentRule))
			{
				return {};
			}

			break;
		case EndMatch:
			if (!request.requestUrl.endsWith(currentRule))
			{
				return {};
			}

			break;
		case ExactMatch:
			if (request.requestUrl != currentRule)
			{
				return {};
			}

			break;
		default:
			if (!request.requestUrl.contains(currentRule))
			{
				return {};
			}

			break;
	}

	const QStringList requestSubdomainList(ContentFiltersManager::createSubdomainList(request.requestHost));

	if (rule->needsDomainCheck && !requestSubdomainList.contains(currentRule.left(currentRule.indexOf(m_domainExpression))))
	{
		return {};
	}

	const bool hasBlockedDomains(!rule->blockedDomains.isEmpty());
	const bool hasAllowedDomains(!rule->allowedDomains.isEmpty());
	bool isBlocked(true);

	if (hasBlockedDomains)
	{
		isBlocked = resolveDomainExceptions(request.baseHost, rule->blockedDomains);

		if (!isBlocked)
		{
			return {};
		}
	}

	isBlocked = (hasAllowedDomains ? !resolveDomainExceptions(request.baseHost, rule->allowedDomains) : isBlocked);

	if (rule->ruleOptions.testFlag(ThirdPartyOption) || rule->ruleExceptions.testFlag(ThirdPartyOption))
	{
		if (request.baseHost.isEmpty() || requestSubdomainList.contains(request.baseHost))
		{
			isBlocked = rule->ruleExceptions.testFlag(ThirdPartyOption);
		}
		else if (!hasBlockedDomains && !hasAllowedDomains)
		{
			isBlocked = rule->ruleOptions.testFlag(ThirdPartyOption);
		}
	}

	if (rule->ruleOptions != NoOption || rule->ruleExceptions != NoOption)
	{
		QHash<NetworkManager::ResourceType, RuleOption>::const_iterator iterator;

		for (iterator = m_resourceTypes.constBegin(); iterator != m_resourceTypes.constEnd(); ++iterator)
		{
			const bool supportsException(iterator.value() != WebSocketOption && iterator.value() != PopupOption);

			if (rule->ruleOptions.testFlag(iterator.value()) || (supportsException && rule->ruleExceptions.testFlag(iterator.value())))
			{
				if (request.resourceType == iterator.key())
				{
					isBlocked = (isBlocked ? rule->ruleOptions.testFlag(iterator.value()) : isBlocked);
				}
				else if (supportsException)
				{
					isBlocked = (isBlocked ? rule->ruleExceptions.testFlag(iterator.value()) : isBlocked);
				}
				else
				{
					isBlocked = false;
				}
			}
		}
	}
	else if (request.resourceType == NetworkManager::PopupType)
	{
		isBlocked = false;
	}

	if (isBlocked)
	{
		ContentFiltersManager::CheckResult result;
		result.rule = rule->rule;

		if (rule->isException)
		{
			result.isBlocked = false;
			result.isException = true;

			if (rule->ruleOptions.testFlag(ElementHideOption))
			{
				result.comesticFiltersMode = ContentFiltersManager::NoFilters;
			}
			else if (rule->ruleOptions.testFlag(GenericHideOption))
			{
				result.comesticFiltersMode = ContentFiltersManager::DomainOnlyFilters;
			}

			return result;
		}

		result.isBlocked = true;

		return result;
	}

	return {};
}

void AdblockContentFiltersProfile::raiseError(const QString &message, ProfileError error)
{
	m_error = error;

	Console::addMessage(message, Console::OtherCategory, Console::ErrorLevel, getPath());

	emit profileModified();
}

void AdblockContentFiltersProfile::handleJobFinished(bool isSuccess)
{
	if (!m_dataFetchJob)
	{
		return;
	}

	QIODevice *device(m_dataFetchJob->getData());

	m_dataFetchJob->deleteLater();
	m_dataFetchJob = nullptr;

	if (!isSuccess)
	{
		raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile: %1").arg(device ? device->errorString() : tr("Download failure")), DownloadError);

		return;
	}

	QTextStream stream(device);
	stream.setCodec("UTF-8");

	const QString header(stream.readLine());

	if (!header.contains(QLatin1String("[Adblock")))
	{
		raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile: invalid header"), ParseError);

		return;
	}

	QByteArray data(header.toUtf8());
	QByteArray checksum;

	while (!stream.atEnd())
	{
		QString line(stream.readLine());

		if (!line.isEmpty())
		{
			if (checksum.isEmpty() && line.startsWith(QLatin1String("! Checksum:")))
			{
				checksum = line.remove(0, 11).trimmed().toUtf8();
			}
			else
			{
				data.append(QLatin1Char('\n') + line);
			}
		}
	}

	if (!checksum.isEmpty() && QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64().remove(22, 2) != checksum)
	{
		raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile: checksum mismatch"), ChecksumError);

		return;
	}

	QDir().mkpath(SessionsManager::getWritableDataPath(QLatin1String("contentBlocking")));

	QSaveFile file(getPath());

	if (!file.open(QIODevice::WriteOnly))
	{
		raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile: %1").arg(file.errorString()), DownloadError);

		return;
	}

	file.write(data);

	m_lastUpdate = QDateTime::currentDateTimeUtc();

	if (!file.commit())
	{
		Console::addMessage(QCoreApplication::translate("main", "Failed to update content blocking profile: %1").arg(file.errorString()), Console::OtherCategory, Console::ErrorLevel, file.fileName());
	}

	clear();
	loadHeader();

	if (m_wasLoaded)
	{
		loadRules();
	}

	emit profileModified();
}

void AdblockContentFiltersProfile::setUpdateInterval(int interval)
{
	if (interval != m_updateInterval)
	{
		m_updateInterval = interval;

		emit profileModified();
	}
}

void AdblockContentFiltersProfile::setUpdateUrl(const QUrl &url)
{
	if (url.isValid() && url != m_updateUrl)
	{
		m_updateUrl = url;
		m_flags |= HasCustomUpdateUrlFlag;

		emit profileModified();
	}
}

void AdblockContentFiltersProfile::setCategory(ProfileCategory category)
{
	if (category != m_category)
	{
		m_category = category;

		emit profileModified();
	}
}

void AdblockContentFiltersProfile::setTitle(const QString &title)
{
	if (title != m_title)
	{
		m_title = title;
		m_flags |= HasCustomTitleFlag;

		emit profileModified();
	}
}

QString AdblockContentFiltersProfile::getName() const
{
	return m_name;
}

QString AdblockContentFiltersProfile::getTitle() const
{
	return (m_title.isEmpty() ? tr("(Unknown)") : m_title);
}

QString AdblockContentFiltersProfile::getPath() const
{
	return SessionsManager::getWritableDataPath(QLatin1String("contentBlocking/%1.txt")).arg(m_name);
}

QDateTime AdblockContentFiltersProfile::getLastUpdate() const
{
	return m_lastUpdate;
}

QUrl AdblockContentFiltersProfile::getUpdateUrl() const
{
	return m_updateUrl;
}

ContentFiltersManager::CheckResult AdblockContentFiltersProfile::evaluateNodeRules(const Node *node, const QString &currentRule, const Request &request) const
{
	ContentFiltersManager::CheckResult result;

	for (int i = 0; i < node->rules.count(); ++i)
	{
		if (node->rules.at(i))
		{
			ContentFiltersManager::CheckResult currentResult(checkRuleMatch(node->rules.at(i), currentRule, request));

			if (currentResult.isBlocked)
			{
				result = currentResult;
			}
			else if (currentResult.isException)
			{
				return currentResult;
			}
		}
	}

	return result;
}

ContentFiltersManager::CheckResult AdblockContentFiltersProfile::checkUrl(const QUrl &baseUrl, const QUrl &requestUrl, NetworkManager::ResourceType resourceType)
{
	ContentFiltersManager::CheckResult result;

	if (!m_wasLoaded && !loadRules())
	{
		return result;
	}

	const Request request(baseUrl, requestUrl, resourceType);

	for (int i = 0; i < request.requestUrl.length(); ++i)
	{
		const ContentFiltersManager::CheckResult currentResult(checkUrlSubstring(m_root, request.requestUrl.right(request.requestUrl.length() - i), {}, request));

		if (currentResult.isBlocked)
		{
			result = currentResult;
		}
		else if (currentResult.isException)
		{
			return currentResult;
		}
	}

	return result;
}

ContentFiltersManager::CosmeticFiltersResult AdblockContentFiltersProfile::getCosmeticFilters(const QStringList &domains, bool isDomainOnly)
{
	if (!m_wasLoaded)
	{
		loadRules();
	}

	ContentFiltersManager::CosmeticFiltersResult result;

	if (!isDomainOnly)
	{
		result.rules = m_cosmeticFiltersRules;
	}

	for (int i = 0; i < domains.count(); ++i)
	{
		result.rules.append(m_cosmeticFiltersDomainRules.values(domains.at(i)));
		result.exceptions.append(m_cosmeticFiltersDomainExceptions.values(domains.at(i)));
	}

	return result;
}

QVector<QLocale::Language> AdblockContentFiltersProfile::getLanguages() const
{
	return m_languages;
}

ContentFiltersProfile::ProfileCategory AdblockContentFiltersProfile::getCategory() const
{
	return m_category;
}

ContentFiltersProfile::ProfileError AdblockContentFiltersProfile::getError() const
{
	return m_error;
}

ContentFiltersProfile::ProfileFlags AdblockContentFiltersProfile::getFlags() const
{
	return m_flags;
}

int AdblockContentFiltersProfile::getUpdateInterval() const
{
	return m_updateInterval;
}

int AdblockContentFiltersProfile::getUpdateProgress() const
{
	return (m_dataFetchJob ? m_dataFetchJob->getProgress() : -1);
}

bool AdblockContentFiltersProfile::create(const QString &name, const QString &title, const QUrl &updateUrl, int updateInterval, ProfileCategory category, QIODevice *rules, bool canOverwriteExisting)
{
	const QString path(SessionsManager::getWritableDataPath(QStringLiteral("contentBlocking/%1.txt")).arg(name));

	if (SessionsManager::isReadOnly() || (!canOverwriteExisting && QFile::exists(path)))
	{
		Console::addMessage(QCoreApplication::translate("main", "Failed to create a content blocking profile: %1").arg(tr("File already exists")), Console::OtherCategory, Console::ErrorLevel, path);

		return false;
	}

	QDir().mkpath(SessionsManager::getWritableDataPath(QLatin1String("contentBlocking")));

	QFile file(path);

	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		Console::addMessage(QCoreApplication::translate("main", "Failed to create a content blocking profile: %1").arg(file.errorString()), Console::OtherCategory, Console::ErrorLevel, file.fileName());

		return false;
	}

	file.write(QStringLiteral("[AdBlock Plus 2.0]\n").toUtf8());
	file.write((QLatin1String("! Title: ") + title + QLatin1Char('\n')).toUtf8());

	if (rules)
	{
		file.write(rules->readAll());
	}

	file.close();

	AdblockContentFiltersProfile *profile(new AdblockContentFiltersProfile(name, title, updateUrl, {}, {}, updateInterval, category, ContentFiltersProfile::NoFlags, ContentFiltersManager::getInstance()));

	ContentFiltersManager::addProfile(profile);

	if (!rules && updateUrl.isValid())
	{
		profile->update();
	}

	return true;
}

bool AdblockContentFiltersProfile::loadRules()
{
	m_error = NoError;

	if (m_isEmpty && !m_updateUrl.isEmpty())
	{
		update();

		return false;
	}

	m_wasLoaded = true;

	if (m_domainExpression.pattern().isEmpty())
	{
		m_domainExpression = QRegularExpression(QLatin1String("[:\?&/=]"));
		m_domainExpression.optimize();
	}

	QFile file(getPath());
	file.open(QIODevice::ReadOnly | QIODevice::Text);

	QTextStream stream(&file);
	stream.readLine(); // header

	m_root = new Node();

	while (!stream.atEnd())
	{
		parseRuleLine(stream.readLine());
	}

	file.close();

	return true;
}

bool AdblockContentFiltersProfile::update(const QUrl &url)
{
	if (m_dataFetchJob || thread() != QThread::currentThread())
	{
		return false;
	}

	const QUrl updateUrl(url.isValid() ? url : m_updateUrl);

	if (!updateUrl.isValid())
	{
		if (updateUrl.isEmpty())
		{
			raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile, update URL is empty"), DownloadError);
		}
		else
		{
			raiseError(QCoreApplication::translate("main", "Failed to update content blocking profile, update URL (%1) is invalid").arg(updateUrl.toString()), DownloadError);
		}

		return false;
	}

	m_dataFetchJob = new DataFetchJob(updateUrl, this);

	connect(m_dataFetchJob, &Job::jobFinished, this, &AdblockContentFiltersProfile::handleJobFinished);
	connect(m_dataFetchJob, &Job::progressChanged, this, &AdblockContentFiltersProfile::updateProgressChanged);

	m_dataFetchJob->start();

	emit profileModified();

	return true;
}

bool AdblockContentFiltersProfile::remove()
{
	const QString path(getPath());

	if (m_dataFetchJob)
	{
		m_dataFetchJob->cancel();
		m_dataFetchJob->deleteLater();
		m_dataFetchJob = nullptr;
	}

	if (QFile::exists(path))
	{
		return QFile::remove(path);
	}

	return true;
}

bool AdblockContentFiltersProfile::resolveDomainExceptions(const QString &url, const QStringList &ruleList) const
{
	for (int i = 0; i < ruleList.count(); ++i)
	{
		if (url.contains(ruleList.at(i)))
		{
			return true;
		}
	}

	return false;
}

bool AdblockContentFiltersProfile::isUpdating() const
{
	return (m_dataFetchJob != nullptr);
}

}
