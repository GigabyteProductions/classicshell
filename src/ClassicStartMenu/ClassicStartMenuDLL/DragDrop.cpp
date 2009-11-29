// Classic Shell (c) 2009, Ivo Beltchev
// The sources for Classic Shell are distributed under the MIT open source license

// DragDrop.cpp - handles the drag and drop functionality of CMenuContainer

#include "stdafx.h"
#include "MenuContainer.h"
#include <algorithm>

// CIDropSource - a basic IDropSource implementation. nothing to see here
class CIDropSource: public IDropSource
{
public:
	// IUnknown
	virtual STDMETHODIMP QueryInterface( REFIID riid, void **ppvObject )
	{
		*ppvObject=NULL;
		if (IID_IUnknown==riid || IID_IDropSource==riid)
		{
			*ppvObject=(IDropSource*)this;
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	virtual ULONG STDMETHODCALLTYPE AddRef( void ) { return 1; }
	virtual ULONG STDMETHODCALLTYPE Release( void ) { return 1; }

	// IDropSource
	virtual STDMETHODIMP QueryContinueDrag( BOOL fEscapePressed, DWORD grfKeyState )
	{
		if (fEscapePressed || (grfKeyState&MK_RBUTTON))
			return DRAGDROP_S_CANCEL;
		if (!(grfKeyState&MK_LBUTTON))
			return DRAGDROP_S_DROP;

		return S_OK;

	}

	virtual STDMETHODIMP GiveFeedback( DWORD dwEffect )
	{
		return DRAGDROP_S_USEDEFAULTCURSORS;
	}
};

///////////////////////////////////////////////////////////////////////////////

LRESULT CMenuContainer::OnDragOut( int idCtrl, LPNMHDR pnmh, BOOL& bHandled )
{
	if (!(m_Options&CONTAINER_DRAG)) return 0;
	NMTOOLBAR *pInfo=(NMTOOLBAR*)pnmh;
	const MenuItem &item=m_Items[pInfo->iItem-ID_OFFSET];
	if (!item.pItem1 || item.id!=MENU_NO) return 0;

	// get IDataObject for the current item
	CComPtr<IShellFolder> pFolder;
	PCUITEMID_CHILD pidl;
	if (FAILED(SHBindToParent(item.pItem1,IID_IShellFolder,(void**)&pFolder,&pidl)))
		return 0;

	CComPtr<IDataObject> pDataObj;
	if (FAILED(pFolder->GetUIObjectOf(NULL,1,&pidl,IID_IDataObject,NULL,(void**)&pDataObj)))
		return 0;

	// force synchronous operation
	CComQIPtr<IAsyncOperation> pAsync=pDataObj;
	if (pAsync)
		pAsync->SetAsyncMode(FALSE);

	// do drag drop
	s_pDragSource=this;
	m_DragIndex=pInfo->iItem-ID_OFFSET;
	CIDropSource src;
	DWORD dwEffect=DROPEFFECT_COPY|DROPEFFECT_MOVE|DROPEFFECT_LINK;
	HRESULT res=SHDoDragDrop(NULL,pDataObj,&src,dwEffect,&dwEffect);

	if (res==DRAGDROP_S_DROP && !m_bDestroyed)
	{
		// if the menu is still open, see if this was a move operation
		bool bMove=false;
		FORMATETC fmt={RegisterClipboardFormat(CFSTR_LOGICALPERFORMEDDROPEFFECT),NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
		STGMEDIUM stgm;
		if (SUCCEEDED(pDataObj->GetData(&fmt,&stgm)))
		{
			DWORD *pdw=(DWORD*)GlobalLock(stgm.hGlobal);
			if (pdw)
			{
				bMove=(*pdw==DROPEFFECT_MOVE);
				GlobalUnlock(stgm.hGlobal);
			}
			ReleaseStgMedium(&stgm);
		}

		if (bMove) // it was a move - the folder was probably modified
		{
			// close all submenus
			for (int i=(int)s_Menus.size()-1;s_Menus[i]!=this;i--)
				if (!s_Menus[i]->m_bDestroyed)
					s_Menus[i]->DestroyWindow();
			// update menu
			PostRefreshMessage();
		}
	}

	// activate the top non-destroyed menu
	for (int i=(int)s_Menus.size()-1;i>=0;i--)
		if (!s_Menus[i]->m_bDestroyed)
		{
			s_Menus[i]->SetActiveWindow();
			break;
		}
	s_pDragSource=NULL;

	return 0;
}

LRESULT CMenuContainer::OnGetObject( int idCtrl, LPNMHDR pnmh, BOOL& bHandled )
{
	NMOBJECTNOTIFY *pInfo=(NMOBJECTNOTIFY*)pnmh;
	if (*pInfo->piid==IID_IDropTarget)
	{
		AddRef();
		pInfo->pObject=(IDropTarget*)this;
		pInfo->hResult=S_OK;
	}
	else
		pInfo->hResult=E_NOINTERFACE;
	return 0;
}

HRESULT STDMETHODCALLTYPE CMenuContainer::DragEnter( IDataObject *pDataObj, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect )
{
	if (m_pDropTargetHelper)
	{
		POINT p={pt.x,pt.y};
		m_pDropTargetHelper->DragEnter(m_hWnd,pDataObj,&p,*pdwEffect);
	}
	m_DropToolbar=NULL;
	if (!m_pParent && m_Items[0].id==MENU_EMPTY && !s_bShowTopEmpty)
	{
		// when dragging over the main menu, show an (Empty) item at the top so the user can drop items there
		s_bShowTopEmpty=true;
		InitToolbars();
	}
	m_DragHoverTime=GetMessageTime()-10000;
	m_DragHoverItem=-1;
	m_pDragObject=pDataObj;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CMenuContainer::DragOver( DWORD grfKeyState, POINTL pt, DWORD *pdwEffect )
{
	grfKeyState&=MK_SHIFT|MK_CONTROL;
	if (grfKeyState==MK_SHIFT)
		*pdwEffect&=DROPEFFECT_MOVE;
	else if (grfKeyState==MK_CONTROL)
		*pdwEffect&=DROPEFFECT_COPY;
	else
		*pdwEffect&=((s_pDragSource && ((s_pDragSource->m_Options&CONTAINER_PROGRAMS) || !s_pDragSource->m_pParent))?DROPEFFECT_MOVE:DROPEFFECT_LINK);

	// only accept CFSTR_SHELLIDLIST data
	FORMATETC format={s_ShellFormat,NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
	if (!m_pDropFolder || !(m_Options&CONTAINER_DROP) || m_pDragObject->QueryGetData(&format)!=S_OK)
		*pdwEffect=DROPEFFECT_NONE;

	POINT p={pt.x,pt.y};
	if (m_pDropTargetHelper)
	{
		m_pDropTargetHelper->DragOver(&p,*pdwEffect);
	}

	// find toolbar under the mouse
	CWindow toolbar=NULL;
	for (std::vector<CWindow>::iterator it=m_Toolbars.begin();it!=m_Toolbars.end();++it)
	{
		RECT rc;
		it->GetWindowRect(&rc);
		if (PtInRect(&rc,p))
		{
			toolbar=*it;
			break;
		}
	}
	if (m_DropToolbar.m_hWnd && m_DropToolbar!=toolbar)
	{
		// clear the insert mark
		TBINSERTMARK mark={-1,0};
		m_DropToolbar.SendMessage(TB_SETINSERTMARK,0,(LPARAM)&mark);
	}

	m_DropToolbar=toolbar;
	if (m_DropToolbar.m_hWnd)
	{
		// mouse is over a button
		m_DropToolbar.ScreenToClient(&p);
		int btnIndex=(int)m_DropToolbar.SendMessage(TB_HITTEST,0,(LPARAM)&p);
		int index=-1;
		if (btnIndex>=0)
		{
			TBBUTTON button;
			m_DropToolbar.SendMessage(TB_GETBUTTON,btnIndex,(LPARAM)&button);
			index=(int)button.dwData-ID_OFFSET;

			// set the new insert mark
			TBINSERTMARK mark={btnIndex,0};
			RECT rc;
			m_DropToolbar.SendMessage(TB_GETITEMRECT,btnIndex,(LPARAM)&rc);
			int y=(rc.top+rc.bottom)/2;
			if (p.y<y)
			{
				// insert above
				if (m_Items[index].id!=MENU_NO && (index==0 || m_Items[index-1].id!=MENU_NO))
					mark.iButton=-1;
			}
			else
			{
				// insert below
				mark.dwFlags=TBIMHT_AFTER;
				if (m_Items[index].id!=MENU_NO && (index==m_Items.size()-1 || m_Items[index+1].id!=MENU_NO))
					mark.iButton=-1;
			}
			m_DropToolbar.SendMessage(TB_SETINSERTMARK,0,(LPARAM)&mark);
			if (mark.iButton==-1 && m_Items[index].bFolder && m_Items[index].bDragInto)
			{
				m_DropToolbar.SendMessage(TB_SETHOTITEM,btnIndex);
			}
			else
			{
				m_DropToolbar.SendMessage(TB_SETHOTITEM,-1);
			}
		}
		else
		{
			// clear the insert mark
			TBINSERTMARK mark={-1,0};
			m_DropToolbar.SendMessage(TB_SETINSERTMARK,0,(LPARAM)&mark);
		}

		// check if the hover delay is done and it's time to open the item
		if (index>=0 && index==m_DragHoverItem)
		{
			if ((GetMessageTime()-m_DragHoverTime)>(int)s_HoverTime && m_Submenu!=m_DragHoverItem && (!m_Items[index].bFolder || m_Items[index].bDragInto))
			{
				// expand m_DragHoverItem
				ActivateItem(index,ACTIVATE_OPEN,NULL);
				if (!m_Items[index].bFolder)
					m_DropToolbar.SendMessage(TB_SETHOTITEM,-1);
			}
		}
		else
		{
			m_DragHoverItem=index;
			m_DragHoverTime=GetMessageTime();
		}
	}
	
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CMenuContainer::DragLeave( void )
{
	if (m_pDropTargetHelper)
		m_pDropTargetHelper->DragLeave();
	if (m_DropToolbar.m_hWnd)
	{
		// clear the insert mark
		TBINSERTMARK mark={-1,0};
		m_DropToolbar.SendMessage(TB_SETINSERTMARK,0,(LPARAM)&mark);
		m_DropToolbar=NULL;
	}
	m_pDragObject.Release();
	return S_OK;
}

HRESULT STDMETHODCALLTYPE CMenuContainer::Drop( IDataObject *pDataObj, DWORD grfKeyState, POINTL pt, DWORD *pdwEffect )
{
	m_pDragObject.Release();

	grfKeyState&=MK_SHIFT|MK_CONTROL;
	if (grfKeyState==MK_SHIFT)
		*pdwEffect&=DROPEFFECT_MOVE;
	else if (grfKeyState==MK_CONTROL)
		*pdwEffect&=DROPEFFECT_COPY;
	else
		*pdwEffect&=((s_pDragSource && ((s_pDragSource->m_Options&CONTAINER_PROGRAMS) || !s_pDragSource->m_pParent))?DROPEFFECT_MOVE:DROPEFFECT_LINK);

	if (m_pDropTargetHelper)
	{
		POINT p={pt.x,pt.y};
		m_pDropTargetHelper->Drop(pDataObj,&p,*pdwEffect);
	}

	if (!m_DropToolbar.m_hWnd) return S_OK;

	TBINSERTMARK mark={-1,0};
	m_DropToolbar.SendMessage(TB_GETINSERTMARK,0,(LPARAM)&mark);
	int before=mark.iButton;
	if (before<0) return S_OK;
	if (mark.dwFlags==TBIMHT_AFTER)
		before++;

	// clear the insert mark
	mark.iButton=-1;
	m_DropToolbar.SendMessage(TB_SETINSERTMARK,0,(LPARAM)&mark);
	m_DropToolbar=NULL;

	if (s_pDragSource==this && *pdwEffect==DROPEFFECT_MOVE)
	{
		// dropped in the same menu, just rearrange the items
		std::vector<SortMenuItem> items;
		for (std::vector<MenuItem>::const_iterator it=m_Items.begin();it!=m_Items.end();++it)
			if (it->id==MENU_NO)
			{
				SortMenuItem item={it->name,it->bFolder};
				items.push_back(item);
			}
		SortMenuItem drag=items[m_DragIndex];
		items.erase(items.begin()+m_DragIndex);
		if (before>m_DragIndex)
			before--;
		items.insert(items.begin()+before,drag);
		SaveItemOrder(items);
		PostRefreshMessage();
	}
	else if (m_pDropFolder)
	{
		// simulate dropping the object into the original folder
		CComPtr<IDropTarget> pTarget;
		if (FAILED(m_pDropFolder->CreateViewObject(NULL,IID_IDropTarget,(void**)&pTarget)))
			return S_OK;
		if (FAILED(pTarget->DragEnter(pDataObj,0,pt,pdwEffect)))
			return S_OK;

		pTarget->DragOver(0,pt,pdwEffect);
		CComQIPtr<IAsyncOperation> pAsync=pDataObj;
		if (pAsync)
			pAsync->SetAsyncMode(FALSE);
		pTarget->Drop(pDataObj,0,pt,pdwEffect);

		std::vector<SortMenuItem> items;
		for (std::vector<MenuItem>::const_iterator it=m_Items.begin();it!=m_Items.end();++it)
			if (it->id==MENU_NO)
			{
				SortMenuItem item={it->name,it->bFolder};
				items.push_back(item);
			}
		items.insert(items.begin()+before,SortMenuItem());
		SaveItemOrder(items);
		PostRefreshMessage();
	}
	return S_OK;
}